# 前向 AD 当前检查点

更新：2026-11-19。以下为最新状态，替代早期“仅完成原语 / 暂无 CLI”的断点。

当前检查点：C0、单点/批量 ABI、DLL、ctypes 与 NumPy 数组基线已完成，53 项 CTest 全部通过。

## 已完成

- 模型级 SSA/切片同步前向 AD，出口变量和 flow 参数梯度。
- 1～64 个 Real 可覆盖参数种子，非光滑和非有限结果带来源诊断。
- 当前 HIR 数学函数局部规则，幂/atan2/tanh 稳定公式。
- effective 独立有效参数偏导；defaults 传播未覆盖默认链。
- 参数/Value 种子支持集，phi/回边固定点和稀疏持久存储。
- 密集参考、模型手算/差分、循环与 CLI 验收。
- 梯度核验 harness 和 gradcheck CLI：同参数模式的中心差分矩阵、范围/路径/步长检查。
- 向量导数 IR：算术 DAG、顺序校验、数学内建、导出并用于实际执行。
- Prepared / mir::Executable：只读校验快照及 C++ 顺序批量接口，含教学示例。
- C0 自包含 C11：原值/梯度、稀疏槽、并行 phi、参数约束、错误与元信息。
- 已实际编译 mos/math/control 三份 DLL，与 C++ 参考逐点对齐。
- C 批量 ABI、Python ctypes、未赋值状态与失败行隔离；localparam 覆盖被拒绝。
- NumPy：直接传入合规数组、雅可比张量、显式复制、输出复用、重叠/布局校验与逐行错误。
- 53 项 CTest 全部通过。记录在 build/windows-msvc/Testing/Temporary/LastTest.log。

## 可复现命令

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'
.\scripts\build.ps1
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o .\build\simple_mos.ad.txt
.\build\windows-msvc\vagrad.exe ad-plan .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM -o .\build\simple_mos.ad-plan.txt
```

详细含义和范围见 ad.md、ad-ir.md、ad-prepared.md。ad-plan 包含支持集、原值 SSA 和向量导数 IR。

## 仍待实施

1. 扩展 C0 默认链/约束表达式/诊断等能力；C0 当前仅 effective 模式。
2. SIMD、内核行间工作区复用、产物哈希缓存；NumPy 已可直接借用合规输入，C 工作区仍逐行分配。
3. 大模型语义覆盖与参数/偏置回归。VA 循环、用户模拟函数和状态算子仍未接通。
4. 优化器、完整电路隐式响应导数。

新增运行入口：

```powershell
python .\python\vagrad_kernel.py .\build\windows-msvc\vagrad_cgen_mos.dll --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300
```

完整说明见 c-codegen.md。build/simple_mos.kernel.json 保存 Python 运行结果，build/simple_mos.batch.csv 保存 C++ 批量结果；generated/ 下 C 和三个 DLL 均由构建产生。

产物核对：build/my_model.c 与回归编译使用的 generated/mos.c 的 SHA-256 一致。build/ad-artifacts.json 保存 C 源码、DLL、可执行文件、结果和测试日志的大小/哈希，用于恢复核对，不能当作已实现的编译缓存。

随后完成 NumPy 基线并更新至 53 项测试；examples/numpy_batch_demo.py 的四点结果保存在 build/simple_mos.numpy.npz，可用 allow_pickle=False 读取。说明见 docs/numpy.md。

## 授权与历史

用户先前要求剩余 1% 时停止，已在当时保存 38 项测试通过的原语检查点；后来明确要求继续直至额度耗尽，又要求继续生成，已恢复开发。后续额度窗口自然更新，未执行额度重置、购买或账户修改。

恢复后形成模型执行/CLI、数学规则/循环验证、参数默认链和稀疏存储。以本文件及 ad.md 的当前状态为准。
