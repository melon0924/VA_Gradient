# MIR-Local 与 CFG 实现说明

2026-11-19，M1/C1 静态标量基线完成，当时全项目 29 项 CTest 通过。后续 SSA 接入后为 33 项，见 [ssa.md](E:/OpenVAF/VA-Gradient/docs/ssa.md)。

当前链路为：

```text
AST View → H1 绑定 → H2 类型化 HIR
                         ├── H3：直接执行 HIR 树
                         └── lower_mir：指令化并拆分基本块
                                   ├── CFG 构建与 MIR 校验
                                   └── MIR 执行器 → 与 H3 对照
```

本说明针对 MIR-Local。这一批没有扩大 Verilog-A 支持范围；后续 SSA/phi 已实现，自动微分仍未实现。

## 1. 直接使用

在 PowerShell 中执行：

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'

# 生成 MIR-Local，--form local 可省略。
.\build\windows-msvc\vagrad.exe mir .\testdata\simple_mos.va --form local -o .\build\simple_mos.mir.txt

# 从 MIR 跳转关系生成 CFG 报告。
.\build\windows-msvc\vagrad.exe cfg .\testdata\simple_mos.va -o .\build\simple_mos.cfg.txt

# 执行 MIR，得到与 H3 相同格式的观察值。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --ir mir-local --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o .\build\simple_mos.mir.eval.txt

# 执行 HIR 参考解释器；原有 eval 不加 --ir 时仍走 HIR。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --ir hir --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300
```

默认模型的实际结构：

| 项目 | 数量 |
|---|---:|
| 基本块 | 10 |
| 局部槽 | 6 |
| 指令结果 Value | 47 |
| 校验 | verified=true |

局部槽包含源码中的 5 个变量，以及处理 && 条件合流的 1 个隐藏临时槽。

MIR、CFG 和执行命令都会自动执行所需的前端步骤。TXT 仅供查看，后续阶段不重新解析 TXT。

## 2. 三种编号分别是什么

| 编号 | 对应对象 | 例子 |
|---|---|---|
| SymbolId | H1 中的源码定义 | 源码变量 ids、参数 G0 |
| LocalId | MIR 中可变的存储槽 | local3 对应 ids |
| ValueId | 某条指令的计算结果 | v12、v36、v43 |
| BlockId | 一组顺序指令及终结指令 | bb0、bb1 |

例如：

```text
v12: Real = binary * v10 v11
store_local local3 v12
...
v36: Real = binary * v26 v35
store_local local3 v36
```

源码中两次写 ids，使用同一个 local3；两次计算各自产生不同的 ValueId。读取 ids 时通过 load_local 得到当前槽值。

**ValueId 有唯一静态定义，但源码变量仍通过 Load/Store 改写。** 本阶段因此称 MIR-Local，尚未将源码变量提升成 SSA；没有 phi。

## 3. HIR 如何变成 MIR

### 顺序表达式与赋值

对 `ids=G0*vds`：

```text
v0: Real = load_param G0
v1: Real = load_local local_vds
v2: Real = binary * v0 v1
store_local local_ids v2
```

参数仍通过 LoadParam 读取，不用默认 G0=0.001 替代。执行时的 ParamEnv 决定本次参数值。

电位读取、温度、显式 Convert、纯数学调用、诊断和 flow 贡献也各有指令。Contribution 不会变成普通 StoreLocal。

### if / else

```verilog
if (c)
    ids = a;
else
    ids = b;
```

降低为：

```text
条件块：计算 c；branch c → then / else
then： 计算 a；store ids；jump merge
else： 计算 b；store ids；jump merge
merge：继续后续语句
```

如果没有 else，假分支直接跳到 merge，保留进入条件前的 ids。基本块编号是分配顺序，不代表执行顺序；执行器必须跟随跳转。

### &&、|| 与三目表达式

`a && b` 先求 a，仅在 a 非零时求 b。两条路径把 0/1 写到同一个隐藏整数槽，汇合后再读取。

`cond ? x : y` 也创建真假块和汇合块，只有选中路径会求值，然后把结果写入隐藏槽。不同于预先计算 x、y 再选择，因此未选中的 sqrt(-1) 或除零不会被执行。

这里先使用临时槽合流，后续 SSA 阶段再考虑用 phi 替换。

## 4. CFG 与校验

CFG 的权威来源是 BasicBlock.terminator：

- Jump 产生一条边。
- Branch 产生两条不同目标的边。
- Return / Unreachable 不产生边。

build_cfg 计算 predecessors、successors、可达性和逆后序。它使用显式 DFS 栈，可处理回边而不无限递归。编辑 MIR 跳转后必须重建分析结果。

verify_mir 检查：

1. 入口、块目标、终结指令形态和来源编号。
2. Value、Local、参数编号是否合法，每个 Value 是否恰有一个定义。
3. 指令参数数量、类型、返回类型、Builtin 签名和探针端点。
4. Effect 指令没有结果，普通数值指令有结果。
5. 同块中定义先于使用；跨块的定义支配使用。
6. 不可达块须显式使用 Unreachable，不能悄悄忽略错误代码。

“定义支配使用”是指：从入口到这个使用点的每一条路径，都必须先经过定义点。若只在 then 块定义 v5，merge 块不能直接使用 v5，因为 else 路径没有计算它。这也是本阶段使用隐藏 Local 槽合流的原因。

校验器使用共用的支配分析检查值可用性。后续已加入支配树、支配边界和局部槽 SSA 提升，见 SSA 说明。校验上限为 2,048 个块和 1,000,000 个 Value，超过时明确报错。

## 5. MIR 执行器如何工作

执行器维护：

- 当前 BlockId。
- 每个 ValueId 最近一次执行所得的数值。
- 每个 LocalId 的可变槽。
- flow 贡献累加器和诊断记录。

执行循环依次解释当前块的指令，再依据终结指令选择下一个块；Return 时收集 module 变量出口值和 flow。它不调用 H3，也不回读 HIR 的语句树。

HIR 和 MIR 共用 runtime/scalar.cpp 中的标量运算、转换及域检查；两者的控制流执行独立。参数有效值计算继续使用现有 ParamEnv。

变量在本次路径读前未赋值会报 EVAL_STATE。缺失输入、除零、非有限结果、整数越界和步数超限继续报错；失败不会输出成功的部分观察值。HIR 和 MIR 的执行步数统计不同，不要求两者相等。

MIR 图执行循环有步数上限，测试覆盖了人工构造的回边图；这不表示 Parser/HIR 已接通 Verilog-A for/while/repeat。

## 6. 数值验收

六组手算点、24 个额外偏置网格点、G0/VTH0/TNOM/L 覆盖、SOFT_THRESHOLD / TRACE_MOS 配置均与 H3 对齐。

在 `Vg=1、Vd=0.05、Vs=0、T=300 K`：

```text
ids@exit      ≈ 2.625e-5 A
flow(ds)@exit ≈ 2.625e-5 A
```

温度改为 600 K，ids 仍约 2.625e-5 A，flow(ds) 约 1.3125e-5 A，与源码的温度缩放公式一致。

测试还检查嵌套短路、字符串、整数运算、贡献正反向、诊断记录的顺序/参数/来源，以及缺失输入和数学错误的类别。数值误差使用 `1e-18 + 1e-12*abs(reference)`。

## 7. 参数与命令边界

`mir` 和 `cfg` 是编译/结构命令，不求参数有效环境，也不接受 --param。即使默认参数存在循环，类型正确的 MIR 仍可生成；实际执行时可以用合法覆盖打断默认依赖。

`eval --ir mir-local` 支持 --param、--voltage、--temperature。所有新命令支持已有 -D / -I / -o 规则，保护主文件和已加载头文件。前端、校验或求值失败时，在打开输出之前退出，保留旧结果。

后续已支持 `--form ssa`、`--ir mir-ssa`，并可用 `cfg --dominators` 查看支配分析。退出码沿用：0 成功，1 前端/校验/执行/输出错误，2 命令格式错误。

## 8. 代码入口与下一步

| 文件 | 职责 |
|---|---|
| include/vagrad/ir/mir/mir.hpp | MIR 与 CFG 数据结构和接口 |
| src/ir/mir/lower.cpp | HIR → MIR-Local |
| src/ir/mir/verify.cpp | CFG、结构/类型/支配校验 |
| src/ir/mir/dump.cpp | MIR 与 CFG 文本导出 |
| include/vagrad/ir/mir/eval.hpp、src/ir/mir/eval.cpp | 基本块参考执行 |
| include/vagrad/runtime/scalar.hpp、src/runtime/scalar.cpp | 共用数值原语 |
| tests/mir_tests.cpp | 初始降低结构 |
| tests/mir_verify_tests.cpp | 正常/畸形图、类型、支配及回边 |
| tests/mir_eval_tests.cpp | HIR/MIR/手算数值对齐 |
| tests/cli_mir_modes.cmake | 命令行与输出保护 |

后续 C2/S1 已完成，见 SSA 说明。下一步为 Q1 目标依赖切片，再进入 AD。
