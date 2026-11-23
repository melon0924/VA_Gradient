# Q1 目标切片断点

## 最新状态：Q1 基线已完成

2026-11-19，最终构建成功，37/37 CTest 通过。代码、CLI、正式说明、导出物及阶段记录均已完成。
最近额度查询：5 小时约剩余 29%，周约剩余 3%；未触发 1% 停止条件，未操作额度重置。

已完成：

- 完整 SSA 上的 variable@exit / flow(named_branch)@exit 目标选择。
- 数据、phi 前驱和后支配控制依赖；循环和无法到达出口区域的保守处理。
- target_inputs / execution_inputs：有效参数、节点电位、温度和探针信息。
- 默认 preserve 与显式 target-only 策略，诊断/贡献/检查的保留或排除记录。
- 输出投影、Value 压缩、MIR 重新校验和切片执行；原 CFG 保留。
- 48 个偏置/温度组合、参数覆盖、宏配置、贡献方向和人工循环的数值对齐。
- CLI、格式错误、旧结果保留及输入保护。

实际结果：

- build/simple_mos.ids.slice.txt：target-only，32/57 条指令，不需要 TNOM 或温度。
- build/simple_mos.flow.slice.txt：target-only，37/57 条，保留 TNOM 与温度。
- build/simple_mos.ids.preserve.slice.txt：默认保真策略，报告额外执行依赖。
- build/simple_mos.ids.slice.eval.txt：默认偏置 ids 约 2.625e-5 A。
- docs/slice.md：命令、算法、策略、输入清单和当前边界。
- build/windows-msvc/Testing/Temporary/LastTest.log：37 项通过。

下一步可进入前向 AD。先选择 Real 有效参数种子、输出和不可光滑函数策略，沿已保留的 CFG/phi 传播导数，再做独立数值校验。
当前没有计算梯度。参数默认环境仍完整验证，CFG 未最小化，切片不输出全局直线求值序；VA 支持范围没有扩展。

以下为本次实施过程记录。

起点：SSA 完成，33 项 CTest 通过。用户的剩余额度 1% 停止约定继续有效。
本次开始时：5 小时约剩余 47%，周约剩余 6%。

实施顺序：

1. 在已校验 SSA 上选 ids@exit 或 flow(branch)@exit。
2. 反向追踪数据、phi 前驱及控制依赖，列出有效参数/电位/环境输入。
3. 保留 CFG，生成可执行切片，区分 preserve 与显式 target-only 效果策略。
4. CLI 导出和执行；验证原 SSA 与切片目标值一致。
5. 正式说明、结果及断点更新。

当前 Q1 已完成，37 项测试通过；未开始 AD。恢复时优先查看本文件最新状态。
