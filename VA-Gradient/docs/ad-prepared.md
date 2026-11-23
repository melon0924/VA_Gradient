# 编译一次，多点求导

Prepared 保存只读 MIR、种子计划、导数 IR 和稀疏支持集。多组偏置或参数卡可复用该快照；每次执行重新建立原值、导数、活动标记、flow 和错误状态。

## 可直接运行的 C++ 示例

examples/forward_ad_demo.cpp 展示完整前端到批量 AD，使用 simple_mos 的 G0/VTH0/TNOM 和 d/g/s 节点：

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'
.\scripts\build.ps1
.\build\windows-msvc\vagrad_ad_demo.exe .\testdata\simple_mos.va
.\build\windows-msvc\vagrad_ad_demo.exe .\testdata\simple_mos.va | Set-Content -LiteralPath .\build\simple_mos.batch.csv -Encoding utf8
```

它不是通用 CSV 读取命令；输入配置在示例的 cases 构造循环中。CMake 默认构建该目标，可用 -DVAGRAD_BUILD_EXAMPLES=OFF 关闭。

## 核心调用

```cpp
auto parameters = hir::resolve_parameters(*ssa.hir);
auto compiled = ad::prepare(ssa, parameters, {"G0", "VTH0", "TNOM"});
// 先检查 parameters.has_errors() 和 compiled.has_errors()。

hir::EvalInputs bias;
bias.node_voltages = {{"d", .05}, {"g", 1.0}, {"s", 0.0}};
bias.temperature_kelvin = 300;
auto result = ad::evaluate(compiled, parameters, bias);

std::vector<ad::BatchCase> cases = {{parameters, bias}};
auto results = ad::evaluate_batch(compiled, cases);
```

上述片段省略命名空间别名和报错展示；完整可编译代码在 examples 中。

单点 evaluate(f, env, inputs, seeds) 保留兼容，内部准备后执行。重复求值时应显式持有 Prepared。

## 哪些数据可以变化

- 节点电位、温度、有效参数数值可以逐次变化。
- 环境必须来自同一个 HIR 的 resolve_parameters；不能混用另一个模块的 SymbolId/ParamEnv。
- 种子名称和顺序、参数导数模式、IR 和切片配置固定在 Prepared 中；改变这些需重新准备。
- defaults 模式中，未选为种子的显式覆盖集合会改变默认链。集合变化时返回 AD_ENV，需重新 prepare。
- 同一覆盖集合的数值可以变化。选中种子本来就独立，是否显式覆盖该种子不改变支持集。

例如对 A 求导，B 默认 3A；无 B 覆盖的快照不能直接用于 --param B=5 的环境。重新准备后可继续改变 B=6、7 等数值。

Prepared 的 accepts 只判断环境归属/形状/覆盖签名，不重新解析默认式。调用方应使用参数解析器提供已校验、内部一致的有效环境。

## 不可变快照与错误隔离

mir::Executable 构造时复制并校验 MIR，随后仅暴露 const 视图。它的执行重载可跳过反复结构/支配分析；普通 evaluate_mir(MirFunction,...) 仍每次验证。

Prepared 持有这个快照，即使调用方后来修改原 MirFunction，也不会改变已准备的执行程序。输入环境只读，每一行独立计算；某行失败会返回该行错误，后续行继续，不把失败行填成成功的零结果。

本文的 evaluate_batch 是 C++ 顺序批量接口，返回与输入顺序一致的 Result 列表。另有 C0/C ABI 和 NumPy，见 c-codegen.md、numpy.md；SIMD、内核行间工作区复用及磁盘缓存仍未实现。

默认实例输出 4 个栅压点，每行包括 ids、对 G0/VTH0 的导数和 flow 对 TNOM 的导数。G0=1m、Vg=1、Vd=.05 时 ids=2.625e-5，dids/dG0=.02625，dids/dVTH0=-5e-5。

## 代码

include/vagrad/ad/prepared.hpp 与 src/ad/prepared.cpp 管理快照；ad/eval.hpp 的重载和 BatchCase 提供执行接口。mir/eval.hpp 的 Executable 封装已验证 MIR。tests/ad_prepared_tests.cpp 验证多点复用、源 IR 隔离、失败行和默认覆盖签名。
