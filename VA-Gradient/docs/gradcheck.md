# 梯度核验：AD 与中心差分矩阵

gradcheck 对每个“数值输出 × 选定参数”生成一行核验结果。它调用现有前向 AD，同时用原值 MIR 执行器计算参数正负扰动后的值；差分不参与 AD 的实现。

## 使用

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'
.\build\windows-msvc\vagrad.exe gradcheck .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM --step G0=1e-7 --step VTH0=1e-5 --step TNOM=.01 --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o .\build\simple_mos.gradcheck.txt
```

也支持 --ir mir-slice、--target、--effects、--param、-D、-I 和 --parameter-derivatives。

默认 h=max(|参数值|×1e-5,1e-12)；可重复 --step NAME=POSITIVE_NUMBER 指定不同参数的物理步长。默认步长只是初值，应按参数单位、尺度及范围调整。实数加减后必须得到不同且有限的 double，否则报告 GRADCHECK_STEP。

差分使用实际浮点间距 (f(p+h)-f(p-h))/((p+h)-(p-h))。默认判据：

```text
|AD-FD| <= atol + rtol × max(|AD|,|FD|)
atol=1e-10, rtol=1e-5
```

可用 --atol、--rtol 调整。过松容差会掩盖误差，过小步长会增加消减误差；不应仅凭一个点的 PASS 判断完整模型正确。

## 两种参数模式

- effective：复制有效参数环境，只扰动本列参数，其他有效值包含 localparam 均固定。重新校验类型、有限性和 from/exclude，不重算默认式。
- defaults：保留原显式覆盖，固定全部选中种子，再扰动当前列种子，重新解析剩余默认式。其他选中种子不会恢复成依赖默认式。

因此 A→B=3A→C=B+1 对 A 的两个核验结果可分别为 0、3，并且都通过各自正确的差分参考。

## 核验状态与报告

每行包含输出、参数、AD、FD、h、绝对误差、容差及 PASS/FAIL。

| 情况 | 退出码 / 输出行为 |
|---|---|
| 所有行匹配 | 0，完整 PASS 报告 |
| 计算成功但存在数值不匹配 | 1，完整 FAIL 报告可保存到 -o |
| 域/范围/路径等导致核验未完成 | 1，标准错误写诊断，不替换旧报告 |
| 命令格式错误、未提供种子 | 2 |

输出继续保护主文件和加载的头文件。错误与完成的不匹配分开处理，避免把一份中断的核验标为完整矩阵。

## 范围与局限

正负扰动都检查参数约束；不静默裁剪步长或改成单边差分。记录原值 MIR 的分支条件编号与选择序列，扰动改变路径时报告 GRADCHECK_PATH，需减小步长或换到远离边界的点。

这份路径记录覆盖 MIR 控制流，不覆盖参数默认式内部或 min/max 等数学内建的所有隐式分段。跨过这些分段通常体现为差分不匹配；测试者仍应选择光滑区域并检查步长稳定性。对复数解析片段的复步微分尚未实现。

代码入口为 include/vagrad/validation/gradcheck.hpp、src/validation/gradcheck.cpp。已有有效环境校验接口在 hir/eval.hpp，供独立有效参数扰动使用。测试覆盖模型全矩阵、切片、默认链、多种子、范围失败、路径变化、数值不匹配和浮点扰动消失。
