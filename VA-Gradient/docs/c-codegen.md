# C0 后端、C ABI 与 Python 调用

2026-11-19：53 项 CTest 通过。C11/DLL 与 C++ AD 原值/梯度对齐，含数学、条件未赋值、单点/批量、ctypes 与可选 NumPy 接口。

## 1. 已有产物直接运行

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'
.\scripts\build.ps1
python .\python\vagrad_kernel.py .\build\windows-msvc\vagrad_cgen_mos.dll --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300
```

输出 JSON 的 outputs 保存数值，jacobian 按输出名、参数名保存导数。默认点 ids=2.625e-5，dids/dG0=.02625，dids/dVTH0=-5e-5。

默认构建会生成三份 C 和对应 DLL：

| 文件 | 用途 |
|---|---|
| build/windows-msvc/generated/mos.c | simple_mos，种子 G0/VTH0/TNOM |
| build/windows-msvc/generated/math.c | 数学函数和实/整常指数幂，种子 P/Q |
| build/windows-msvc/generated/control.c | 条件未赋值、Integer 控制、localparam |
| build/windows-msvc/vagrad_cgen_mos.dll | 上述 mos.c 的动态库 |
| build/windows-msvc/vagrad_cgen_math.dll | 数学参考库 |
| build/windows-msvc/vagrad_cgen_control.dll | 控制参考库 |

构建选项 BUILD_TESTING=OFF 时不自动生成这些回归样例 DLL，emit-c 命令仍可使用。

## 2. 为自己的受支持模型生成 C

```powershell
.\build\windows-msvc\vagrad.exe emit-c .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM -o .\build\my_model.c
```

支持 -D、-I、--param，也可使用 --ir mir-slice、--target、--effects。无须传入偏置；生成代码在运行时接收偏置。

--param 决定生成文件中的默认**有效参数快照**，不会把种子引用折叠成常数。当前 C0 仅支持 effective 模式，--parameter-derivatives defaults 会被拒绝。

自定义 C 可在已有 MSVC x64 环境中独立编译，不依赖本项目头文件：

```powershell
cmd.exe /d /c 'call "E:\OpenVAF\tools\vs-buildtools\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /TC /std:c11 /O2 /fp:precise /utf-8 /LD "E:\OpenVAF\VA-Gradient\build\my_model.c" /Fe:"E:\OpenVAF\VA-Gradient\build\my_model.dll" /Fo:"E:\OpenVAF\VA-Gradient\build\my_model.obj" /link /IMPLIB:"E:\OpenVAF\VA-Gradient\build\my_model.lib"'
```

不使用 fast-math。DLL 架构须与 Python 进程一致。本机验证为 Windows x64。默认导出同名 vagrad_* 符号，每个模型用独立动态库加载。

## 3. C ABI 布局

标量接口：

```c
int vagrad_eval(
    const double* parameters, const double* voltages,
    double temperature, size_t step_limit,
    double* outputs, double* jacobian, uint8_t* valid,
    size_t* error_token);
```

- parameters 按 parameter_name 元信息顺序，长度 parameter_count；NULL 使用编译时快照。它是完整有效环境，包含 localparam，C 调用方须保持环境含义；Python 封装禁止覆盖 localparam。
- voltages 按 node_name 顺序。NaN 代表未提供，实际读取时报告缺失；温度同理。明确提供的非正温度或无穷输入被拒绝。
- outputs / valid 长度 output_count。变量保持其名字，支路采用 flow:名字。
- jacobian 为 output_count×seed_count，按行排列：jacobian[row*seed_count+lane]。
- error_token 返回展开 token 编号，供前端来源映射定位；不是原始文件的行号。
- 调用方按元信息分配足够空间，输入/输出缓冲区不重叠。当前 ABI 无显式缓冲区长度参数。

元信息包括各类 count/name、default_parameter、parameter_overridable，以及 parameter_type（Real=0、Integer=1，越界=-1）。C0 仅支持数值类型。

批量接口：

```c
int vagrad_eval_batch(
    size_t n, const double* parameters, const double* voltages,
    const double* temperatures, size_t step_limit,
    double* outputs, double* jacobian, uint8_t* valid,
    int* statuses, size_t* error_tokens);
```

所有数组按样本行连续排列；parameters 为 n×parameter_count，voltages 为 n×node_count，outputs 为 n×output_count，jacobian 为 n×output_count×seed_count。参数 NULL 时每行用默认快照。

函数返回值表示批量分派是否有效；各行成败在 statuses 中。某行失败不阻止后续行。n=0 允许空指针。当前逐行调用，未做 SIMD，也未复用行间工作区。

| 状态码 | 含义 |
|---:|---|
| 0 | 成功 |
| 1 | ABI 参数错误 |
| 2 | 参数类型、有限性或 from/exclude 约束 |
| 3 | 电位/温度输入缺失或非法 |
| 4 | 原值或导数域错误、非有限值 |
| 5 | 未赋值读取 |
| 6 | 活动非光滑/离散边界 |
| 7 | 执行步数超限 |
| 8 | 工作区分配失败 |

成功但未赋值的出口变量 valid=0，数值/导数为 NaN；它不等于零。执行失败时所有出口失效，避免发布部分结果。

## 4. Python 单点与批量

```python
import sys
sys.path.insert(0, r"E:\OpenVAF\VA-Gradient\python")
from vagrad_kernel import Kernel

model = Kernel(r"E:\OpenVAF\VA-Gradient\build\windows-msvc\vagrad_cgen_mos.dll")
result = model.evaluate(
    parameters={"G0": 0.001},
    voltages={"d": .05, "g": 1, "s": 0},
    temperature=300)
print(result["outputs"]["ids"])
print(result["jacobian"]["ids"]["G0"])

rows = model.evaluate_batch([
    {"voltages": {"d": .05, "g": vg, "s": 0}, "temperature": 300}
    for vg in [.3, .7, 1.0, 1.2]
])
```

单点错误抛 KernelError，带 code/token；批量失败行返回 error 字典，其余行保持结果。未知名称、localparam 覆盖及非法 step_limit 在 Python 层拒绝。

本文件的 Kernel 封装只依赖 ctypes，会复制 Python 数值至 C 数组。另有 NumpyKernel 可直接传入合规数组，见 numpy.md。C0 仍使用独立有效参数语义，改动 A 不重算其他默认式；默认链传播继续使用 C++ AD / CLI defaults。

## 5. 实现与边界

原值 SSA 降低为 C 基本块和跳转，phi 先读取再并行提交。稀疏导数槽由支持集分配；局部导数 DAG 按校验顺序生成 C，数学函数使用随产物一起输出的 C 导数辅助函数。

支持当前静态标量算术、比较/逻辑、转换、数学函数、电位/温度、命名支路贡献。C0 不支持 String/诊断调用、可变 Local、位操作等；范围表达式限字面量、参数引用、转换与正负号，更复杂形式明确报 CGEN_UNSUPPORTED。不生成半份 C 文件，不覆盖输入/旧成功产物。

已验证的 C 样例均为前端支持的无 VA 循环模型；不能将后端 phi 处理称为 VA 循环语义已完成。未实现完整 BSIM、OSDI、ngspice 接口、优化器、SIMD 或磁盘哈希缓存。

源码入口：include/vagrad/codegen/c.hpp、src/codegen/c.cpp、src/codegen/c_runtime.hpp、python/vagrad_kernel.py。tests/c_codegen_tests.cpp 直接链接生成 DLL，与 C++ 参考逐点比较；Python 测试检查调用和失败行隔离。
