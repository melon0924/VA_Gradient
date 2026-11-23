# MIR 实施断点

后续 Q1 已完成，37 项测试通过。最新状态见 docs/slice-checkpoint.md；下文保留原阶段记录。

后续 C2/S1（SSA 与 phi）已完成，33 项测试通过。最新阶段请读取 docs/ssa-checkpoint.md 和 docs/ssa.md；下文为 M1/C1 历史记录。

## 最新状态：M1/C1 静态标量基线已完成

用户继续后已从下方历史断点恢复，补齐所有当时待办，完成编译与 **29/29 CTest**。
最近额度查询：5 小时窗口剩余约 77%，周窗口剩余约 11%，未操作额度重置。
本次任务已完成，未达到新的 1% 停止阈值。

已完成：

- HIR → MIR-Local 降低，保留参数引用、可变 Local、Value 和 Origin。
- CFG 前驱/后继、可达性、逆后序；结构/类型/单一定义/支配校验。
- MIR 与 CFG 导出；独立基本块执行器，与 H3 数值及错误类别对齐。
- 六组独立手算点、24 个偏置网格点、参数覆盖、宏配置、诊断与贡献方向。
- 修复裸 Net/Branch 表达式的 HIR 类型诊断，移除 Void 调用的额外占位常量。
- 共用数值原语抽取到 include/vagrad/runtime/scalar.hpp / src/runtime/scalar.cpp，控制流执行仍独立。
- mir、cfg、eval --ir hir|mir-local 命令及输入覆盖保护。
- README、总计划、docs/mir.md 和开发记录已同步。

实际结果：

- build/simple_mos.mir.txt：10 个块、6 个 Local、47 个 Value，verified=true。
- build/simple_mos.cfg.txt：CFG 与逆后序。
- build/simple_mos.mir.eval.txt：默认点 ids 和 flow(ds) 均约 2.625e-5 A。
- build/windows-msvc/Testing/Temporary/LastTest.log：最终 29 项通过。

下一步 **C2/S1**：支配树、支配边界、Local 槽提升、phi、重命名、SSA 校验和数值对齐。
当前没有 SSA、phi、切片或 AD；没有扩大 VA 循环/用户函数/状态运算的执行范围。
继续工作时先读 docs/mir.md、include/vagrad/ir/mir/mir.hpp 和本节，无需重做下方已完成步骤。

以下为前次 1% 停工记录，保留用于追溯。

日期：2026-11-19。用户要求在剩余额度降至 1% 时保存断点并停止。

## 本次起点

- H1/H2/H3 静态标量基线完成；23 项 CTest 通过。
- H2：include/vagrad/frontend/hir/typed.hpp、src/frontend/hir/lower.cpp。
- H3：include/vagrad/frontend/hir/eval.hpp、src/frontend/hir/eval.cpp。
- CLI：tools/vagrad_main.cpp；构建：scripts/build.ps1。
- 总方案：docs/hir-mir-cfg-plan.md。

## 本次实施顺序

1. MIR-Local：ValueId、LocalId、BasicBlock、指令、终结指令。
2. HIR → MIR：赋值、分支、短路和条件表达式。
3. 从终结指令派生 CFG，校验编号、类型与值的支配关系。
4. 编译、测试、CLI 导出。
5. MIR 独立控制流执行器，与 H3 做数值对齐。

## 前次停工状态（已恢复）

**已按用户要求停止：5 小时窗口已用 99%，剩余约 1%；周窗口已用 86%。未操作额度重置。**

本次已完成并验证：

- include/vagrad/ir/mir/mir.hpp：ValueId、LocalId、BlockId、Instruction、Terminator、BasicBlock、MirFunction；CfgInfo 仅为数据结构预留。
- src/ir/mir/lower.cpp：lower_mir 接收 shared_ptr<const HirModule>，创建 MIR-Local。
- 源码变量通过 LoadLocal/StoreLocal 保留可变槽；参数保留 LoadParam。
- if、&&、||、?: 拆成基本块，保持选中路径求值；表达式合流暂用隐藏 Local 槽。
- 顺序语句、转换、数学调用、电位/温度读取和 flow 贡献均有降低分支。
- CMake 已接入编译，并添加 tests/mir_tests.cpp。
- 执行 .\scripts\build.ps1 成功，**24/24 CTest 通过**，包含原有 23 项回归。
- 新 MIR 结构测试检查分支/合流块、终结指令、目标编号、Value 单一定义、参数引用及隐藏槽。

**这是 MIR 降低的第一检查点，M1/C1 尚未完整验收。** 目前没有 mir/cfg CLI 命令，没有 MIR TXT，也没有 MIR 数值执行器；不能声称与 H3 数值对齐。

## 前次恢复待办（本次已完成）

1. 实现 CFG：由 Terminator 唯一派生前驱、后继、可达性及逆后序；补齐 CfgInfo::has_errors（目前仅声明）。
2. 实现 verify_mir：编号、类型、终结指令、操作数定义及支配关系；为畸形 MIR 加负面测试。
3. 审查 lowering 的异常路径：当前有效 HIR 中若出现裸 Net/Branch 引用的表达式语句，locals_.at 可能抛异常；应在 HIR 或 MIR 明确诊断。
4. Void 诊断调用目前额外发出一个未使用的常量占位；可在表达式/语句入口整理，但需保持副作用顺序。
5. 加 MIR/CFG 文本导出与 CLI，继续复用 tools/vagrad_main.cpp 的前序链及输入覆盖保护。
6. 实现独立基本块执行器；可共享数值运算原语，不共享 H3 的控制流执行方式。
7. 对 simple_mos 默认和宏配置、六组手算点、参数覆盖、短路、贡献方向与错误状态做 HIR/MIR 对齐。
8. 再更新 README、总计划和正式 MIR 说明；这些文件目前仍显示上个 H2/H3 已完成版本。

本次没有修改 HIR 支持范围，没有增加循环、SSA、AD 或自动重置额度。

## 恢复原则

先查看本文件的最终状态及 CMakeLists.txt，不重新创建前端。
在 E:\OpenVAF\VA-Gradient 执行 .\scripts\build.ps1，确认上次构建结果。
MIR 消费已完成的 HirModule，保留 Origin 与参数引用，不读取导出 TXT。
首版保持现有 HIR 执行能力，不顺带扩展循环、函数、SSA 或 AD。
