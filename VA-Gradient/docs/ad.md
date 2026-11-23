# 前向 AD：参数导数与稀疏执行

2026-11-19：当前 53 项 CTest 全部通过。已接通模型级梯度、默认链、稀疏存储、核验、导数 IR、快照、C0、ctypes 与 NumPy。

## 1. 这一阶段解决什么问题

给定模型参数与端电位，程序先计算 ids、支路贡献，再计算它们对指定实数参数的导数。例如：

```verilog
parameter real G0 = 1m;
parameter real VTH0 = .45;
real vov, ids;
analog begin
    vov = V(g,s) - VTH0;
    ids = G0 * (vov * V(d,s) - .5 * V(d,s) * V(d,s));
end
```

在这条线性区表达式中，V(g,s)=1、V(d,s)=.05 时：

- vov=.55，ids=2.625e-5。
- ∂ids/∂G0 = vov×.05 - .5×.05² = .02625。
- ∂ids/∂VTH0 = G0×(-1)×.05 = -5e-5。

前向 AD 用链式法则得到这些结果。中心差分仅用于测试参考。

它计算的是**给定端电位下模型输出对参数的导数**。当前没有求解电路工作点，也没有把端电压随参数变化的电路响应计入导数。还未接入 LM/Adam。

## 2. 从命令行输出导数

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'

# --wrt 自动使用 MIR-SSA，列顺序与指定顺序一致。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o .\build\simple_mos.ad.txt

# target-only 去掉 ids 不需要的温度计算。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --ir mir-slice --target ids@exit --effects target-only --wrt G0 --wrt VTH0 --voltage d=.05 --voltage g=1 --voltage s=0 -o .\build\simple_mos.ids.ad.txt

# 静态种子支持集，不需要端电位或温度；报告附原值 SSA 便于核对编号。
.\build\windows-msvc\vagrad.exe ad-plan .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM -o .\build\simple_mos.ad-plan.txt
```

种子必须是 1～64 个不同的、可覆盖的 Real module 参数。Integer、localparam、未知名和重复名会被拒绝。--wrt 仅配合 mir-ssa/mir-slice；省略 --ir 自动选择 mir-ssa。

默认稀疏存储；--ad-storage dense 保留密集参考。输出是文本检查报告，运行时 IR 和梯度在内存中传递。失败不替换旧结果；输出也不能覆盖本次加载的主文件或头文件。

## 3. 结果与雅可比矩阵

默认模型、Vg=1、Vd=.05、Vs=0、T=300 K：

| 观察值 | 原值 | 对 G0 的导数 | 对 VTH0 的导数 | 对 TNOM 的导数 |
|---|---:|---:|---:|---:|
| ids | 2.625e-5 | .02625 | -5e-5 | 0 |
| flow(ds) | 2.625e-5 | .02625 | -5e-5 | 8.75e-8 |

每个输出为一行、每个种子为一列，右侧三列就是雅可比矩阵。文本形式为：

```text
d(ids)/d(G0) = 0.02625
d(ids)/d(VTH0) = -5e-05
d(flow:ds)/d(TNOM) = 8.75e-08
```

ids 的表达式没有 TNOM，所以导数为零；flow 使用 ids×TNOM/T，其 TNOM 导数为 ids/T。

在不跨越路径边界、变化足够小时：

```text
Δids ≈ .02625 × ΔG0 - 5e-5 × ΔVTH0
```

优化器以后可使用这一线性近似选择参数步长。导数本身不决定完整优化步骤。

## 4. 两种参数含义

### effective：独立有效参数，默认模式

完整样例在 testdata/semantic/ad_parameter_chain.va：

```verilog
parameter real A = 2;
parameter real B = 3*A;
localparam real C = B+1;
real ids;
analog ids = C;
```

解析后 A=2、B=6、C=7。effective 只给选中的参数放置单位种子，其他有效值固定。因此对 A 求偏导时 B/C 固定，d(ids)/d(A)=0。数值参考也须固定其他有效参数，不能让解析器重算所有默认式。

### defaults：传播默认表达式

```powershell
.\build\windows-msvc\vagrad.exe eval .\testdata\semantic\ad_parameter_chain.va --wrt A --parameter-derivatives defaults
```

沿 A → B=3A → C=B+1 传播，d(ids)/d(A)=3。

规则：

1. 选中参数是彼此独立的种子，其自身默认式不继续展开。
2. 显式覆盖、但未选为种子的参数固定。
3. 其余参数与 localparam 沿默认式传播，支持传递依赖与实际选中分支。
4. 原值来自已校验的 ParamEnv，导数不改变原执行路径。
5. 仅在参数实际加载时计算其导数；无关默认式的导数奇异性不阻止输出。

显式 --param B=5 后，再对 A 使用 defaults，B 固定，结果为零。对 A、B 同时 --wrt 时，B 也是独立种子，此例导数列为 [0,1]。

defaults 的差分参考应扰动种子、重新解析未覆盖默认值，同时固定其他显式覆盖和其他种子。两个参数模式的参考不能混用。

## 5. SSA Value 如何携带导数

局部数学规则使用：

```cpp
struct Jet {
    double value;
    std::vector<double> tangent; // 按种子顺序排列
    bool active;                // 是否依赖种子，与数值梯度是否为零分开
};
```

以 G0、VTH0 为种子：

- 加载 G0：[1,0]；加载 VTH0：[0,1]。
- 给定电位、常量、环境温度：[0,0]。
- 减法：d(a-b)=da-db。
- 乘法：d(a*b)=b*da+a*db。
- phi：选择实际前驱的向量，同一块全部 phi 先读取再提交。
- flow：按端点方向累计贡献导数。
- Return：收集当前成功路径上有定义的数值观察值。

原 MIR 执行器计算原值，ExecutionObserver 在同一路径同步传播导数，不重新解释 AST。

活动标记不能只看向量是否非零。P=0 时 P*P 的导数为零，但 sqrt(P*P) 仍有奇异/非光滑依赖，需要诊断。max 若实际选中常量，则该分支可保持非活动。

## 6. 稀疏支持集与存储

50 个参数中，某 Value 若只依赖 G0、VTH0，持久存储只需这两个分量。

分析顺序：

1. effective 仅参数种子本身有支持集；defaults 沿未覆盖默认式的引用传播。
2. SSA 的操作数和 phi 输入合并支持集。
3. 工作队列迭代到固定点，覆盖回边。

64 位掩码表示“可能依赖哪些种子”，是保守上界。候选分支通常取并集，不能因为本次梯度恰好为零就删掉分量。

TangentState 只持久保存掩码分量。局部 Jet 临时使用完整长度向量；输出保留全部种子列。没有分量的已定义零，与未赋值 Undef 分开记录。掩码外出现非零导数会报错，不静默丢弃。

```text
Tangent storage=sparse slots=稀疏上界/密集上界
```

这一计数是所有 SSA Value 的潜在 double 分量数，**不等于总内存或当前路径峰值**，也不能当作速度提升倍数；仍有原值、活动标记、容器、临时 Jet 和输出。持久导数分量上限为 8,000,000。

ad-plan 输出种子支持、原值 SSA 和向量导数 IR。后者与原值指令配对，并非全部展开到原 MirFunction 的标量导数，也不是机器码。保留 CFG；含回边时不宣称有全局指令拓扑序。详见 ad-ir.md。

## 7. 数学与边界策略

覆盖 HIR 的实数 +、-、*、/、**，以及：

- min、max、abs、sqrt、exp、ln、log（以 10 为底）。
- sin、cos、tan、asin、acos、atan、atan2。
- sinh、cosh、tanh、asinh、acosh、atanh。
- ceil、floor，以及 analog 表达式中的 $vt(T)。

ceil/floor 远离整数跳变点时导数为零，活动参数在整数点时拒绝。$vt(T) 对显式温度参数传播；环境温度不是本次种子。HIR 仍禁止 $vt 出现在参数默认式。

保守拒绝包括：活动 min/max/abs 模糊边界、比较/直接条件切换点、sqrt(0)、反函数奇异端点、活动 Real→Integer、非有限导数，以及零/负底数不满足本实现规则的活动幂。负底数的整常指数按对应规则处理。

相同 SSA 操作数可识别为同值，但不做额外符号等价证明，故可能保守拒绝实际光滑的等价表达式。当前无用户指定次梯度政策。

幂、atan2、tanh 使用避免部分溢出/下溢损失的传播公式，仍受双精度范围限制。原值域错误沿用 ScalarOps 诊断。VA 语义仍是 HIR 的静态标量子集，完整状态算子/用户模拟函数未实现。

## 8. 代码与验收

| 文件 | 职责 |
|---|---|
| src/ad/forward.cpp | 局部链式规则、数学边界 |
| src/ad/plan.cpp | 种子验证、有效参数 Jet |
| src/ad/parameters.cpp | 默认链、覆盖边界、定位错误 |
| src/ad/sparsity.cpp | 参数/Value 支持集和固定点 |
| src/ad/state.cpp | 稀疏存储、Undef、并行 phi |
| src/ad/eval.cpp | 同步传播、活动标记、flow、雅可比 |
| src/ir/mir/eval.cpp | 原值执行与观察回调 |
| tests/ad_tests.cpp | 基础规则、模型手算 |
| tests/ad_rules_tests.cpp | 数学规则差分、边界、稳定公式 |
| tests/ad_integration_tests.cpp | 偏置网格、密集/稀疏、人工循环、64 种子 |
| tests/ad_parameter_tests.cpp | 默认链、覆盖、多种子、来源与差分 |
| tests/cli_ad_modes.cmake | CLI、计划导出、模式、文件保护 |

人工 MIR 循环验证零/一/多次迭代和交换 phi，不等于 VA 循环语法已接通 HIR。

## 9. 后续

已提供 gradcheck 命令进行逐输出/种子的中心差分核验，见 gradcheck.md。该工具区分两个参数模式，检查范围、MIR 路径、浮点步长与误差；没有用差分替代 AD。

向量导数 IR、Prepared、C++ 批量、C0 和 ctypes 已完成，NumPy 数组接口见 numpy.md。C0 当前不支持 defaults 模式；后续扩大模型覆盖并推进 SIMD、工作区复用与缓存。实际 BSIM 语义仍逐步验收。
