# SSA 实施断点

后续 Q1 已完成，37 项测试通过。最新状态见 docs/slice-checkpoint.md；下文保留原阶段记录。

## 最新状态：C2/S1 基线已完成

2026-11-19，最终构建成功，33/33 CTest 全部通过。说明文档和阶段记录已同步。
最近查询：5 小时窗口约剩余 51%，周窗口约剩余 6%；未触发 1% 停止条件，未使用额度重置。

交付物：

- docs/ssa.md：算法、phi 例子、未赋值语义、校验、执行与运行命令。
- build/simple_mos.ssa.txt：10 块、42 个 Value、3 个 phi，verified=true。
- build/simple_mos.ssa.eval.txt：SSA 数值结果，与 HIR/MIR-Local 对齐。
- build/simple_mos.dominance.txt：CFG、idom、支配树子节点和支配边界。
- build/ssa_branch.ssa.txt：小型 phi 阅读样例。
- build/windows-msvc/Testing/Temporary/LastTest.log：最终测试记录。

下一步为 **Q1 目标依赖切片**。以 SSA Return 的 ids@exit 或指定 flow 贡献为根，保留数据依赖、phi 的前驱边及控制依赖，先检查切片前后数值一致，再进入 AD。
当前没有构建目标切片，没有计算参数梯度；VA 循环/用户函数/状态运算的支持范围未扩展。

以下为本次实施过程记录，恢复时优先读取本节和 docs/ssa.md。

当前任务：C2/S1，支配分析、phi、重命名、校验和三种执行形式对齐。
用户的剩余额度 1% 停止约定继续生效；初始查询周窗口约剩余 10%。

已验证起点：MIR-Local/CFG 完成，29 项 CTest 通过。

本次顺序：

1. 支配集合、直接支配者、支配树、支配边界。
2. 为 Local 构造 phi、沿支配树重命名、删除 Load/Store 并压缩 Value。
3. phi 前驱边校验，SSA Return 出口值与未赋值语义。
4. 基本块执行器并行计算 phi，CLI 支持 ssa / mir-ssa。
5. 结构/循环/畸形图/数值及 CLI 测试，说明文档与导出物。

当前：SSA 核心、CLI、正式文档及导出物已完成，33 项 CTest 已通过。
默认 simple_mos：10 个块、42 个 Value、3 个 phi，Local 的 Load/Store 已移除。
已验证六组手算点、偏置网格、宏配置、缺失输入、未赋值路径、循环交换的并行 phi。
人工循环图还验证入口前置块、支配边界、回边和 SSA 执行步数上限。
实现入口：src/analysis/dominance.cpp、src/ir/mir/ssa.cpp；校验和执行扩展原文件。
CLI：mir --form ssa、eval --ir mir-ssa、cfg --dominators。
当前尚未开始切片或 AD。恢复时先检查本文件最新内容。
正式代码仍在 include/vagrad/ir/mir、src/ir/mir；分析放入 analysis。
不扩展 VA 循环或函数语法，不在本任务中实现 AD。
