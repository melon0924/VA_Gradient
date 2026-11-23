# SSA 与 phi 构造说明

2026-11-19 完成 C2/S1 静态标量基线，当时 33 项 CTest 全部通过。后续 Q1 已完成，全项目为 37 项，见 [slice.md](E:/OpenVAF/VA-Gradient/docs/slice.md)。

本次在已有 MIR-Local 上增加支配分析、活跃变量分析、phi 插入和重命名，移除 Local 的 Load/Store，并让 SSA 与 HIR、MIR-Local 对照执行。

## 1. 使用命令

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'

# 构造并校验 SSA。
.\build\windows-msvc\vagrad.exe mir .\testdata\simple_mos.va --form ssa -o .\build\simple_mos.ssa.txt

# 查看直接支配者、支配树子节点和支配边界。
.\build\windows-msvc\vagrad.exe cfg .\testdata\simple_mos.va --dominators -o .\build\simple_mos.dominance.txt

# 执行 SSA，输入方式与之前相同。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --ir mir-ssa --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o .\build\simple_mos.ssa.eval.txt

# 更小的 phi 阅读样例。
.\build\windows-msvc\vagrad.exe mir .\testdata\semantic\ssa_branch.va --form ssa -o .\build\ssa_branch.ssa.txt
```

现有 `mir --form local`、`eval --ir hir`、`eval --ir mir-local` 和默认 eval 行为仍保留。

默认 simple_mos 的结构：

| 形式 | 基本块 | Value | phi | 可变 Local Load/Store |
|---|---:|---:|---:|---|
| MIR-Local | 10 | 47 | 0 | 保留 |
| MIR-SSA | 10 | 42 | 3 | 已全部移除 |

SSA 报告仍显示 6 个 Local 的定义元信息，用于解释原变量和隐藏合流槽；执行 SSA 时不分配这些可变槽。Value 数量也不是优化质量指标，目前没有做全面常量传播或死代码消除。

## 2. 用一个实际例子理解 phi

`testdata/semantic/ssa_branch.va` 中：

```verilog
parameter real G = 1m;
parameter integer ENABLE = 1;
real ids;
analog begin
    if (ENABLE)
        ids = 2.0 * G;
    else
        ids = 3.0 * G;
    I(ds) <+ ids;
end
```

SSA 导出的主要部分：

```text
bb0:
  v1: Integer = load_param ENABLE
  branch v1 ? bb1 : bb2
bb1:
  ... v4 = 2.0 * G
  jump bb3
bb2:
  ... v7 = 3.0 * G
  jump bb3
bb3:
  v8: Real = phi local0 [bb1: v4] [bb2: v7]
  require_defined local0 v8
  contribute_flow (d, s) alias=ds v8
  return ids@exit=v8
```

对应含义：

- 从 bb1 进入 bb3，v8 取 v4。
- 从 bb2 进入 bb3，v8 取 v7。
- 每次实际执行只选一个前驱输入；phi 不求平均，也不把两条支路相加。
- v8 对应本次执行路径汇合后的 ids，Return 明确将它记录为 ids@exit。

原来的两个 StoreLocal 被替换为两条路径中各自的值；汇合点用 phi 表达“后续应该使用哪一个值”。

`local0` 在 phi 和 require_defined 中是来源元信息，不代表执行时再次读写 Local 槽。require_defined 是项目保留的未赋值检查，详见第 5 节。

## 3. 构造的具体步骤

### 支配关系

如果从入口到 B 的每一条路径都经过 A，则 A 支配 B。

直接支配者 idom(B) 是离 B 最近的严格支配者。这些关系构成支配树；入口没有直接支配者。

支配边界 DF(A) 描述 A 的影响与其他路径汇合的位置：某块的至少一个前驱被 A 支配，但该块本身没有被 A 严格支配。

在上例中，bb1 和 bb2 都以 bb3 为支配边界。变量的不同定义因此可能需要在 bb3 合流。

分析输出包含：

```text
bb3 ... idom=bb0 frontier=[ ... ] children=[ ... ]
```

支配树与普通 CFG 不同：CFG 描述可能跳到哪里，支配树描述到达某处必经哪些块。

### 活跃变量与 phi 插入

先记录每个 Local 在各块中的定义和读取，再反向计算 live-in / live-out。Return 对 module 变量的观察也计入使用。

对每个 Local，从其 Store 所在块开始，沿迭代支配边界寻找合流点；只有该 Local 在合流点 live-in 时才插入 phi。这是使用活跃性裁剪的 SSA 构造。

因此，命名块内一个之后再也不读的私有临时量，不会仅因为在 if/else 中被赋值就生成无用 phi。

### 沿支配树重命名

实现为每个 Local 维护一条“当前定义栈”：

1. 进入块时，把该块 phi 的结果压栈。
2. 遇到 Store，把右侧 ValueId 压栈，删除 Store。
3. 遇到 Load，用栈顶 ValueId 替换它的使用，保留必要的未赋值检查。
4. 将当前定义填入后继块 phi 中对应本块的输入。
5. 递归意义上处理支配树子块，离开块后恢复栈。

代码使用显式遍历栈，避免长支配树消耗 C++ 调用栈。

然后折叠“所有非自引用输入都指向同一个 Value”的冗余 phi，压缩 Value 编号，再运行完整校验。不同参数加载或不同公式即使数值偶然相等，也不会仅凭当前默认值合并。

参数 G0 等仍为 LoadParam，来源 Origin 和预处理来源图继续保留，尚未固定成默认数值。

## 4. SSA 校验与执行

校验器在原有 MIR 检查之上要求：

- phi 位于块首，先于普通指令。
- 每个 phi 对每个 CFG 前驱恰有一个输入，不多、不少、不重复。
- 输入、结果和原 Local 类型一致。
- phi 输入的定义在相应前驱出口可用；它是前驱边上的使用。
- 普通 Value 使用满足支配关系，同块中定义先于使用。
- SSA 中不能残留 LoadLocal / StoreLocal。
- Return 对每个 module 变量都有明确、类型正确的出口值。

执行器依据实际前驱选择 phi 输入。一个块的所有 phi 先读取前驱状态，再同时提交结果；这是循环中交换两个变量等情形保持正确的关键。

例如一对循环 phi：

```text
x = phi [entry: 1], [backedge: y]
y = phi [entry: 2], [backedge: x]
```

从回边进入时应得到交换后的 x、y。若先覆盖 x 再读取它来计算 y，就会错误地把两个变量变成同一个值。测试用人工 MIR 循环专门检查了这一点。

SSA 的“单赋值”指每个 ValueId 只有一个静态定义；循环里同一条 phi 可以在不同迭代反复执行。

## 5. 未赋值路径如何处理

本项目的 H3/MIR 静态求值基线要求变量在本次实际路径中读前赋值，不模拟完整电路分析的跨调用历史。SSA 必须保留这个行为。

例如：

```verilog
if (ENABLE)
    ids = 1.0;
// ENABLE 为 0 时，本次还没有为 ids 赋值。
```

实现使用 Undef 指令，报告显示 `unassigned`，表示“本次尚未赋值”。这是本项目的状态标记，不采用任意数值语义，也不把缺失输入补成 0。

phi 可以选择并传播这个标记：

- 真正读取未赋值 Value 时，require_defined 或数值使用报 EVAL_STATE。
- 若只是在 Return 观察一个本次未赋值的 module 变量，仍按 H3 规则省略该观察项。
- 即使独立表达式语句的读取结果被丢弃，原本的未赋值错误也不会因删除 Load 而消失。

## 6. 回边与范围

带回边的入口会增加前置块，使初次进入也有明确的 phi 前驱。不可达块继续保留 Unreachable，SSA 转换明确清除其中不可执行的普通指令。

当前通过人工 MIR 图验证循环携带值、并行 phi、入口前置块和执行上限。这不表示已经扩展 Verilog-A for/while/repeat 的 HIR 支持范围。

资源上限沿用 2,048 个基本块、1,000,000 个 Value；SSA 活跃性矩阵另限制为 8,000,000 个条目。超限会诊断。

## 7. 已完成的验收

全项目 33 项 CTest 通过，新增验收覆盖：

- 支配关系、直接支配者、支配树和支配边界。
- 菱形分支、嵌套短路、字符串合流和冗余 phi 消除。
- 默认 simple_mos 的六组独立手算点、24 个偏置网格点和参数覆盖。
- HIR / MIR-Local / MIR-SSA 的数值、输出键、诊断记录与错误类别对齐。
- SOFT_THRESHOLD / TRACE_MOS 配置、贡献方向和温度缩放。
- 人工循环执行 0/1/2/3/7 次，检查两个变量的交换与循环计数。
- phi 缺失/重复/非法输入、边上非支配使用、块首顺序和缺失 Return 输出。
- 命令行模式、上游错误阻断、旧结果保留和输入文件保护。

## 8. 源码入口与下一步

| 文件 | 作用 |
|---|---|
| include/vagrad/analysis/dominance.hpp | 支配分析结果接口 |
| src/analysis/dominance.cpp | 支配集合、idom、支配树、支配边界 |
| src/ir/mir/ssa.cpp | 活跃性、phi 插入、重命名、简化及编号压缩 |
| include/vagrad/ir/mir/mir.hpp | MirForm、PhiInput、ReturnValue 和新指令 |
| src/ir/mir/verify.cpp | 普通使用和 phi 边使用的校验 |
| src/ir/mir/eval.cpp | 并行 phi、未赋值状态与 SSA Return |
| tests/ssa_tests.cpp | 分析、转换、畸形图和三种形式数值对齐 |
| tests/cli_ssa_modes.cmake | 实际 CLI 与文件保护 |

下一阶段是 Q1：从 ids@exit 或指定 flow 贡献出发建立目标依赖切片，保留必要的 phi、数据依赖和控制依赖，再进入 AD。当前尚未计算参数导数。
