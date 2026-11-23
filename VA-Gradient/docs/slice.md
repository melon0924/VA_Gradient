# Q1：SSA 目标依赖切片

2026-11-19，Q1 静态标量基线完成，37 项 CTest 通过。

切片从已校验 SSA 的出口观察值或指定 flow 贡献开始，反向保留计算目标所需的定义、phi 输入和控制依赖。结果仍是一份可执行 SSA，并继续保留 CFG。

## 1. 直接运行

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'

# 提取 ids 的目标计算，明确选择只保留目标所需计算。
.\build\windows-msvc\vagrad.exe slice .\testdata\simple_mos.va --target ids@exit --effects target-only -o .\build\simple_mos.ids.slice.txt

# 提取 flow(ds)，PowerShell 中将带括号的目标加引号。
.\build\windows-msvc\vagrad.exe slice .\testdata\simple_mos.va --target 'flow(ds)@exit' --effects target-only -o .\build\simple_mos.flow.slice.txt

# 执行 ids 切片，不需要传入温度。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --ir mir-slice --target ids@exit --effects target-only --voltage d=.05 --voltage g=1 --voltage s=0 -o .\build\simple_mos.ids.slice.eval.txt

# 执行 flow 切片，需传入温度。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --ir mir-slice --target 'flow(ds)@exit' --effects target-only --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 600
```

各命令自动执行前端和 SSA 构造，不读取以前导出的 TXT。slice 是编译命令，不接受参数覆盖；eval 支持原有 --param、-D、-I、-o 规则。

## 2. 当前样例的实际结果

| 目标，target-only 模式 | 保留指令 / 原 SSA 指令 | 目标参数 | 温度 |
|---|---:|---|---|
| ids@exit | 32 / 57 | G0、VTH0、LEAK、ENABLE | 不需要 |
| flow(ds)@exit | 37 / 57 | 上述参数和 TNOM | 需要 |

两者都读取 d、g、s 的电位，L 不出现在这些目标的必要输入中。ENABLE 为 Integer 控制参数，不能直接当作连续可微实数参数。

原因仍来自原公式：ids 先计算，随后贡献 `ids / ($temperature / TNOM)`。以 ids 为根时，温度缩放不在其数据路径中。

在 Vg=1、Vd=0.05、Vs=0：

- ids 切片得到约 2.625e-5 A。
- flow 切片在 600 K 得到约 1.3125e-5 A。

这些是目标值，尚未生成参数导数。

## 3. 输入清单怎样阅读

报告开头分别列出：

```text
target_inputs parameters=[ ... ] nodes=[ ... ] temperature=...
execution_inputs parameters=[ ... ] nodes=[ ... ] temperature=...
```

target_inputs 是目标的数据与控制依赖。execution_inputs 是生成的可执行切片实际保留的全部依赖，可能更大。

参数清单表示 SSA 直接加载的有效参数。完整 ParamTable、默认值依赖和范围校验仍由原 HIR/ParamEnv 保存与处理；本阶段没有裁剪参数默认环境。

因此，不在清单中出现的 L 仍保留在完整模型元信息中。若主动覆盖 L，其原有参数合法性检查仍适用。

## 4. 副作用与错误策略

| 策略 | 行为 |
|---|---|
| preserve，默认 | 保留原输入读取、可能报错的计算、未赋值检查、诊断和全部 flow 贡献 |
| target-only，显式选择 | 提取目标计算，允许排除无关诊断、未选中的贡献和无关报错计算 |

例如 ids 的默认 preserve 切片仍保留 flow 的温度读取，所以执行时仍需温度；其 target_inputs 不含温度，execution_inputs 则含温度。

target-only 的输出只包含选定变量或选定 flow。它针对目标做数值提取，不承诺复现被明确排除的程序行为。例如无关的 sqrt(-1) 可以被排除，而目标自身路径中的 sqrt(-1) 仍报错。

报告中每条诊断、贡献和未赋值检查都有 Effect 行，注明原块、指令位置、来源 token 和 retained 状态。排除依据是明确策略，不会仅因指令没有返回值就静默忽略。

执行步数按各自程序计数，可能不同；临界步数预算不作为数值等价判据。

## 5. 切片如何实现

1. 对完整 SSA 运行校验，建立 ValueId 到定义指令的索引。
2. 变量目标从所有可达 Return 中的对应值开始；flow 目标从同一端点对的所有贡献开始，包含反向贡献。
3. 反向追踪普通操作数和 phi 的前驱输入。
4. 使用后支配关系取得控制依赖，把决定路径和 phi 选择的条件纳入目标依赖。
5. 按效果策略补充执行检查与副作用。
6. 删除无关指令、缩小出口观察项、压缩 Value 编号并重新校验。

原 CFG 的块和跳转保持保留，因此全部分支条件仍纳入 execution_inputs。某个与目标无关的控制条件可能因此成为额外执行输入；当前不进行最小 CFG 重构。

如果图中存在无法到达出口的区域，控制依赖采用保守策略，报告 conservative_control=1。循环保留回边与 phi；cfg_cycle=1 时尤其不能把切片当成全局无环公式。即使无环，存在分支时也不意味着可以把所有指令一次性顺序求值。

## 6. 验收与当前边界

新增测试覆盖 48 个电压/温度组合、参数覆盖、phi 控制条件、贡献方向、TRACE_MOS / SOFT_THRESHOLD、未赋值出口、循环及执行前后目标值对齐。

默认 preserve 模式检查原输入和数学错误仍保留；显式 target-only 检查无关行为按策略排除。CLI 检查目标/策略格式、旧结果保留和输入文件保护。

本阶段只支持 module 变量出口及命名 branch 的 flow 出口。不支持任意源码行观察点、内部时刻、任意表达式目标、最小 CFG 化或完整模型语义扩展。

下一步可在已验证切片上接入前向 AD，先选择 Real 参数种子，处理 phi、控制路径和不可光滑函数的导数策略，再以差分等方法验收。当前尚未计算梯度。

## 7. 代码与结果

| 文件 | 内容 |
|---|---|
| include/vagrad/ir/mir/slice.hpp | SliceResult、输入清单和效果策略 |
| src/ir/mir/slice.cpp | 控制依赖、反向标记、过滤、重编号和报告 |
| mir.hpp / verify.cpp / eval.cpp | 输出投影及切片执行 |
| tests/slice_tests.cpp | 结构、依赖、效果和数值验收 |
| tests/cli_slice_modes.cmake | CLI 验收 |
| docs/slice-checkpoint.md | 本阶段完成状态与下一步 |
