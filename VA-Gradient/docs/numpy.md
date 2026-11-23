# NumPy 批量原值与雅可比接口

当前验证环境为本机 NumPy 2.3.5，使用已有安装，无新增下载。vagrad_numpy.py 是独立可选封装，原来的标准库 ctypes 接口继续可用。

## 1. 运行示例

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'
.\scripts\build.ps1
python .\examples\numpy_batch_demo.py .\build\windows-msvc\vagrad_cgen_mos.dll --output .\build\simple_mos.numpy.npz
```

示例运行四个 Vg 点 .3、.7、1、1.2，Vd=.05、Vs=0、T=300 K，得到：

| Vg | ids | ∂ids/∂G0 | ∂ids/∂VTH0 |
|---:|---:|---:|---:|
| .3 | 5e-14 | 0 | 0 |
| .7 | 1.125e-5 | .01125 | -5e-5 |
| 1 | 2.625e-5 | .02625 | -5e-5 |
| 1.2 | 3.625e-5 | .03625 | -5e-5 |

NPZ 保存数值数组及输出/种子名称，不包含 Python object；可用 np.load(path, allow_pickle=False) 读取。

## 2. Python 用法

```python
import sys
import numpy as np
sys.path.insert(0, r"E:\OpenVAF\VA-Gradient\python")
from vagrad_numpy import NumpyKernel

model = NumpyKernel(r"E:\OpenVAF\VA-Gradient\build\windows-msvc\vagrad_cgen_mos.dll")
gates = np.array([.3, .7, 1., 1.2], dtype=np.float64)
volts = np.empty((gates.size, len(model.nodes)), dtype=np.float64)
for name, values in {"g": gates, "d": .05, "s": 0}.items():
    volts[:, model.nodes.index(name)] = values

result = model.evaluate_arrays(
    voltages=volts, temperatures=np.full(gates.size, 300.)
).require_all()

ids = result.observation("ids")               # (samples,)
dids_dG0 = result.derivative("ids", "G0")     # (samples,)
ids_jacobian = result.jacobian[:, model.outputs.index("ids"), :]
print(result.values.shape)                   # (4, 6)
print(result.jacobian.shape)                 # (4, 6, 3)
```

所有列按动态库元信息 model.parameters、model.nodes、model.outputs、model.seeds 排列，不应假设源码声明顺序或硬编码列号。

## 3. 输入约定

| 参数 | 形状 / 含义 |
|---|---|
| parameters | (samples, parameter_count)，完整有效参数；None 使用编译快照 |
| voltages | (samples, node_count)，按节点元信息排列 |
| temperatures | (samples,)，kelvin；None 表示未提供 |
| count | 一般由数组推断；全部输入为 None 时必需 |
| copy | 默认 False，严格要求直接可传给 C 的布局 |
| out | 可选、形状/元信息匹配的 ArrayResult，用于复用输出缓冲区 |
| step_limit | 每行执行步数上限 |

参数矩阵包含所有有效参数列，不仅是导数种子。localparam 必须保持编译快照中的值，修改会在封装层被拒绝。其他参数逐行接受 C0 类型/范围检查。

不隐式广播一维参数或标量温度；统一参数可用 parameters=None，温度可显式 np.full(samples, 300.)。各输入的样本数必须一致。

copy=False 要求 NumPy ndarray、native float64、C 连续且对齐；只读输入可以直接传入。非连续切片、float32、非本机字节序或未对齐数组会被拒绝。copy=True 会按需要转换/复制，但不会修改形状规则。

## 4. 输出与错误

| 字段 | 形状 | 类型 |
|---|---|---|
| values | (samples, outputs) | float64 |
| jacobian | (samples, outputs, seeds) | float64 |
| valid | (samples, outputs) | uint8 |
| status | (samples,) | intc，与 C int 匹配 |
| tokens | (samples,) | uintp，与 size_t 匹配 |

result.ok 返回逐行成功掩码。require_all() 在任一行失败时抛 KernelError；它不会把成功但未赋值的观察量当成执行失败。未赋值输出 valid=0、数值/导数为 NaN，使用前仍需检查该列 valid。

失败行保留状态和 token，并失效全部输出；其他行继续。状态码与 C ABI 文档相同。

## 5. 直接传入与复用的边界

合规输入使用原 NumPy 数据指针传给 C；不先转换成 Python 列表或另建 ctypes 数值数组。copied_inputs 记录接口是否做过输入复制。

这只表示入参可以直接借用，**不代表整个调用零分配**：输出默认新建，C 内核仍逐行分配内部工作区，当前也没有 SIMD。

```python
# 重复调用可复用输出数组；返回同一个对象，其内容被新结果覆盖。
again = model.evaluate_arrays(voltages=volts,
    temperatures=np.full(gates.size, 300.), out=result)
assert again is result
```

out 必须形状、类型、元信息匹配，且可写、对齐、C 连续。输出之间以及输出与输入之间不能重叠，封装会检查。调用期间不得从其他线程修改输入或复用同一输出缓冲区。

## 6. 后续拟合如何取矩阵

若残差定义为 I_model-I_measured，且测量电流不依赖模型参数，则该残差的雅可比就是：

```python
row = model.outputs.index("ids")
J = result.jacobian[:, row, :]
residual = result.values[:, row] - measured_current
```

当前 MOS 的 ids 对 TNOM 恒为零，对应列不能用于识别 TNOM；flow:ds 的 TNOM 导数才非零。先确认目标和参数的依赖关系，再交给优化器，不能把任意输出的矩阵混用。

本阶段没有接入优化器。C0 仍使用独立有效参数模式，不沿默认式重算其他参数；C++ defaults 模式是另一种导数语义。

## 7. 验证

新增 python_numpy 测试，覆盖原值/梯度、直接借用、只读输入、非连续/未对齐数组、显式复制、输出复用、重叠拒绝、批量失败、空批次、未定义输出和 localparam。当前 53 项 CTest 全部通过；未安装 NumPy 时跳过该可选测试，不影响核心 C++/C 和标准库绑定。
