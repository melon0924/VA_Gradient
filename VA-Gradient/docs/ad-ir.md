# 导数 IR：把链式规则变成可校验的程序

2026-11-19 完成向量导数 IR 基线，已接入模型求导执行路径。

## 输入和位置

输入是已校验的 MIR-SSA。每个原值 vN 对应一个向量导数定义 d(vN)，向量列对应用户选定的种子。原值 CFG 继续负责分支与循环；导数定义与相应的原值指令关联，不另建可能不同步的控制图。

```text
MIR-SSA
  ├── 种子支持集：哪些列需要持久保存
  └── DerivativeIr：这些值的导数如何计算
        ↓ 校验
原值执行器 + 导数 IR 执行 + 稀疏 TangentState
```

这是一种与原值 SSA 配对的**向量导数 IR**，不是将每个标量导数全部插入原 MirFunction 的形式。数学函数保留受检查的导数内建规则。后续 C0 已将这些规则生成到自包含 C11 辅助函数，并编译成 DLL，见 c-codegen.md。

## 乘法：实际生成什么

若原值指令为 v8 = v3 * v5，则导数定义为：

```text
d(v8) = expression * [v3, d(v3)] [v5, d(v5)]
  q0 = tangent_arg v3
  q1 = tangent_arg v5
  q2 = primal_arg v3
  q3 = primal_arg v5
  q4 = mul q0 q3
  q5 = mul q2 q1
  q6 = add q4 q5
  yield q6
```

q0 是 d(v3)，q3 是原值 v5，所以 q4=d(v3)×v5；q5=v3×d(v5)；q6 即乘法法则。

这里的 v8/v3/v5 仅示意编号；实际编号见导出的原值 SSA。q 编号是**当前规则内部**的标量临时节点，每个规则重新从 q0 编号。执行每一列种子时沿同一局部 DAG 计算，不将 q0 当作整个函数的 SSA ValueId。

## 除法与原值复用

d(a/b) 使用 (da-y×db)/b，其中 y 为已有原值结果 a/b。这样无需显式形成 b²，降低部分中间溢出风险，并保持与现有局部传播原语相同的运算顺序。

PrimalResult 节点引用刚完成的原值指令结果；不会重算整个源表达式。节点执行检查有限性和除零，不做浮点重结合。

## 规则类别

| 类别 | 含义 |
|---|---|
| zero | 参数无关常量、电位或环境输入等零导数 |
| undef | 未赋值状态，不能当作零 |
| parameter_seed | 从有效参数/默认链环境取得种子向量 |
| expression | 加减乘除、实数符号和安全转换的局部 DAG |
| phi | 根据原值 CFG 的实际前驱并行选择导数 |
| discrete_guard | 整数/比较/布尔结果的零导数及活动边界检查 |
| chain_unary / chain_binary | 数学函数导数内建规则 |
| chain_power | 幂的域、活动性和稳定传播规则 |
| select_extremum | min/max 的选中分支导数及相等边界策略 |

flow 不产生普通 Value 定义；导出中明确记录按原端点方向累计输入导数的操作。Return 收集同一原值 SSA 出口的导数。

## 折叠顺序的硬检查

verify_derivatives 检查：

1. IR 的 Value 数、定义、源 opcode、类型、操作数和操作符与原 SSA 对应。
2. 局部节点参数数目正确，叶节点指向合法操作数。
3. 每个子节点编号严格小于当前节点，根节点存在且在范围内。
4. 非表达式规则不能混入局部 DAG。

自引用、前向引用和缺失根会产生 AD_IR 错误，阻止执行。该顺序只针对**局部无环表达式**；整个函数含 phi/回边时仍沿 CFG 执行，不声称存在全局直线拓扑序。

目前不执行会改变语义的参数默认值常量折叠，也不擅自将活动性等同于数值导数非零。边界规则和来源诊断沿用前向 AD。

## 导出与阅读

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'
.\build\windows-msvc\vagrad.exe ad-plan .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM -o .\build\simple_mos.ad-plan.txt
```

报告依次包含种子支持集、原值 MIR-SSA、Derivative IR。读取某个 d(vN) 时，先在原值部分找到 vN，确认其操作数，再看局部 q 节点。

源码入口为 include/vagrad/ad/ir.hpp、src/ad/ir.cpp；src/ad/eval.cpp 已执行生成的普通导数规则，参数/phi 仍由各自环境与 CFG 适配层处理。测试比较编译后的算术 DAG 与直接 Jet 原语，拒绝畸形顺序，并对当前数学函数和模型输出做中心差分。
