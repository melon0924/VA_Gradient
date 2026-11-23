# HIR H2 / H3 实现说明

2026-11-19 完成当前 simple_mos 静态标量基线，当时 23 项 CTest 通过；后续 MIR/SSA 接入后全项目为 33 项。

H2 把 AST View 和 H1 名称绑定转换成有类型、有明确电学含义的结构化 HIR；H3 直接解释这份 HIR，得到给定参数和偏置下的变量出口值与支路贡献。

## 1. 先运行这两条命令

在 PowerShell 中执行：

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'

# H2：类型、参数表、表达式和结构化语句，保存为文本。
.\build\windows-msvc\vagrad.exe hir .\testdata\simple_mos.va --typed -o .\build\simple_mos.typed.hir.txt

# H3：给出节点电位与温度，执行 HIR，保存数值结果。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --voltage d=0.05 --voltage g=1 --voltage s=0 --temperature 300 -o .\build\simple_mos.eval.txt
```

在这个偏置点，输出应接近：

```text
HIR H3 status=ok
ids@exit = 2.625e-05
flow(ds)@exit = 2.625e-05
```

实际文本保留 17 位精度，因此可能显示 2.6250000000000004e-05 等二进制浮点表示。

原来的 `hir` 命令仍输出 H1 绑定报告；只有加 `--typed` 才进入 H2。两者都自动执行前面的预处理、解析、CST 和名称绑定，不需要读取先前导出的 TXT。

## 2. 代码之间如何衔接

```text
.va + include + 宏配置
  → PreprocessResult
  → ParseResult → SyntaxTree → AST View
  → bind_names() → BindingResult（H1）
  → lower_hir()  → HirModule（H2）
                     ├── parameters：参数默认表达式、属性、约束
                     ├── expressions：有类型的表达式 arena
                     ├── statements：Block / If / Assign / Contribute ...
                     └── bindings：继续共享 H1 与源码来源图
  → resolve_parameters() → ParamEnv
  → evaluate_hir(HirModule, ParamEnv, EvalInputs) → EvalResult（H3）
```

HirModule 中的 ExprId / StmtId 是数组索引。它们分别编号表达式和语句，既不是 SSA ValueId，也不是基本块编号。MIR/CFG/phi 属于后续独立对象；当前项目尚未实现自动微分。

TXT 是便于检查的导出物。真正的数据通过 C++ 对象传递，H3 不解析 H2 的 TXT，也不回读 AST 重做控制流解释。

## 3. H2 具体做了什么

### 类型与字面量

Type 区分 Real、Integer、String、Net、Branch、Void、Error。条件使用数值零/非零语义，比较结果为 Integer 的 0/1。

| 源码 | 解码结果 | 类型 |
|---|---:|---|
| 1m | 0.001 | Real |
| 1u | 0.000001 | Real |
| 3.0e2 | 300 | Real |
| 8'h0F | 15 | Integer 字面量，另存 width=8、radix=16 |
| 8'shFF | -1 | Integer 字面量，另存显式 signed |

数值解码不受系统区域设置的小数点形式影响，要求消费整个字面量；非法尾部、x/z/?、非有限数及超出能力的宽度均诊断。Real 使用 double；执行期 Integer 使用有符号 int32。

必要的 Integer / Real 转换作为 Convert 节点记录。Real 转 Integer 取最近整数，恰好一半时远离零；整数除法截断向零。超出 int32 范围会报错。

**位宽边界：** 支持 1～32 位 based 字面量的解码和显式 integer / real 参数初始化；不完整实现 Verilog 的上下文位宽与无符号运算。直接对 based 整数字面量做整数运算、单目数值/位运算、整数条件合并，或省略其参数类型，会被诊断。可以先声明 `parameter integer MASK=8'h0F;`，再在 int32 运算中使用 MASK。超宽、截断及无法表示的无符号大数也会报错。

Net 和 Branch 是电学对象，不能参与普通数值算术。参数不能作为赋值目标。数组、用户模拟函数和其他未支持语义不会被当作实数执行。

### ParamInfo 与 ParamEnv

ParamInfo 保存定义、类型、默认 ExprId、是否允许覆盖、属性原文及来源、from/exclude 约束。所有属性都可查询，但 units 只是元信息，目前不进行量纲分析。

ParamEnv 是给定一组覆盖后求出的有效值。规则是：

1. 检查覆盖名字、类型和可覆盖性，localparam 不允许外部覆盖。
2. 对被覆盖参数直接采用覆盖值，不执行其默认表达式。
3. 对其余参数按依赖递归求值，并检测有效默认依赖中的循环。
4. 全部值就绪后验证范围。多个 from 是并集，exclude 从允许集合中排除；保留开闭端点，inf 为专用边界标记。

例如 `testdata/semantic/parameter_dependencies.va`：

```verilog
parameter real G0 = 1m;
parameter real SCALE = 2;
parameter real EFFECTIVE_GAIN = G0 * SCALE;
```

默认 EFFECTIVE_GAIN=0.002。覆盖 G0=0.002 后，EFFECTIVE_GAIN 的默认表达式重新计算为 0.004。输出分别展示默认环境与有效环境，两份结果不会相互改写。

```powershell
.\build\windows-msvc\vagrad.exe eval .\testdata\semantic\parameter_dependencies.va --param G0=2m --voltage d=1 --voltage s=0
```

如果默认环境自身有循环，而合法覆盖打断了循环，可以得到 `default_status=error effective_status=ok`，有效环境仍能执行。无法推导类型的无类型循环会在 H2 提前诊断。

**参数使用处始终保留 Ref(SymbolId)。** G0 的默认值虽是 0.001，其每次使用仍是参数引用，为后续 LoadParam 与参数求导保留依赖。

### Probe / Contribution / Builtin

| 原 AST | H2 表达 |
|---|---|
| G0 | Ref(SymbolId(G0)): Real |
| V(g,s) | ReadPotential(positive=g, negative=s): Real |
| V(ds) | 按 branch 定义解析端点并保留 alias |
| V(a) 或 V(a,0) | 负端为参考地 |
| $temperature | Temperature: Real，输入单位 K |
| max(a,b) | PureMathCall，按登记的参数类型和数量检查 |
| ids=expr | Assign，覆盖源码变量槽 |
| I(ds)<+expr | ContributeFlow，累加静态 flow 贡献 |

V(a,b) 使用同一组节点电位计算 Va−Vb，因此 V(b,a) 符号相反。输入无需另外提供每个探针的差分值。

BuiltinRegistry 登记函数参数规则、参数数量、返回类型及效果类别。数学函数支持 min/max、sqrt、exp、ln（自然对数）、log（十进对数）、三角及双曲函数等；$vt 可使用显式正温度或环境温度。

$strobe / $display / $write 收集函数名、已求值参数及来源，不尝试复现仿真器的格式化和调度。ddt/idt/limexp/噪声/transition 等状态或分析相关运算给出能力诊断。

每个 HIR 表达式和语句保留 Origin，关联 CST NodeId 与展开 token。错误继续沿用预处理器的 include / 宏来源图。

## 4. H3 如何执行

输入为参数有效环境、节点电位、可选温度。读取需要的电位或温度时，如果未提供，就报 EVAL_INPUT；用不到的温度可以省略。传入未知节点名字会报错。

- Block 按源码顺序执行。
- If 先计算条件，再执行选中分支。
- Assign 更新对应 SymbolId 的局部槽，后续读取取得最近一次已执行的赋值。
- ContributeFlow 累加到规范端点对，反向端点带负号；命名 branch 也能查询同一贡献和。
- && / || / 三目表达式按所选路径求值，未选中的纯表达式不会产生除零或域错误。

模块变量在真实模拟器中可能跨求值保留状态。这里限定无历史的单点评估：每次读变量前，必须先在本次实际执行路径中赋值。未赋值读取报 EVAL_STATE，避免把历史相关代码错误地当成每次清零。

成功后，EvalResult 提供：

| 字段 | 内容 |
|---|---|
| observations | module 作用域中实际已赋值的变量出口值 |
| branch_flows | 命名 branch 方向下的静态贡献和 |
| endpoint_flows | 包括无名支路在内的规范端点对贡献和 |
| records | 诊断函数的参数记录与来源 |
| diagnostics / steps | 错误状态和执行步数 |

这里的 flow 是模型给出的贡献，不是通过 KCL/KVL 求解的完整电路电流。当前不读取 I(...)，也不执行电压源贡献 V(...)<+...。调用者提供节点偏置，H3 不求解电路。

除零、数学域/溢出导致的非有限值、整数溢出、缺失输入和步数超限均给出带来源的错误。失败不发布部分观察值为成功数值；CLI 的失败 eval 保留此前的输出文件。

## 5. 已通过的独立数值验收

默认 G0=0.001、VTH0=0.45、LEAK=1e-12、TNOM=300，s=0：

| Vgs / V | Vds / V | 温度 K | ENABLE | ids@exit / A | flow(ds)@exit / A |
|---:|---:|---:|---:|---:|---:|
| 0.3 | 0.05 | 300 | 1 | 5e-14 | 5e-14 |
| 1.0 | 0.05 | 300 | 1 | 2.625e-5 | 2.625e-5 |
| 1.0 | 0.8 | 300 | 1 | 1.5125e-4 | 1.5125e-4 |
| 1.0 | 0.55 | 300 | 1 | 1.5125e-4 | 1.5125e-4 |
| 1.0 | 0.05 | 300 | 0 | 5e-14 | 5e-14 |
| 1.0 | 0.05 | 600 | 1 | 2.625e-5 | 1.3125e-5 |

测试将这些手算数值写成独立参考常量，误差判据为 `abs_error <= 1e-18 + 1e-12*abs(reference)`。

最后一行两种电流不同，是因为源码先计算 ids，然后贡献 `ids / ($temperature / TNOM)`。温度改变不会自动修改源码里没有温度依赖的 ids 表达式。

其他测试覆盖 G0/VTH0/TNOM 覆盖、L 无关性、宏配置、诊断记录、端点反向、贡献累加、参数依赖和循环、范围、类型、短路、错误状态及 CLI 输入保护。

## 6. 常用命令与退出码

```powershell
# 参数覆盖；输出源码公式在指定输入下的数值。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --param VTH0=0.6 --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300

# 替代宏分支及诊断调用。
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va -DSOFT_THRESHOLD -DTRACE_MOS --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300

# 参数依赖的 H2 报告。
.\build\windows-msvc\vagrad.exe hir .\testdata\semantic\parameter_dependencies.va --typed --param G0=2m -o .\build\parameter_dependencies.typed.hir.txt

# 故意触发 H2 语义错误 / H3 除零。
.\build\windows-msvc\vagrad.exe hir .\testdata\semantic\typed_errors.va --typed
.\build\windows-msvc\vagrad.exe eval .\testdata\semantic\runtime_errors.va
```

CLI 的 --param 接收数字，可重复指定不同参数；字符串参数覆盖可通过 C++ API 的 Scalar{std::string} 提供。--voltage 接收节点电位而非 Vgs/Vds 的别名，--temperature 接收 K。所有后续命令继续支持 -D、-I、-o。

退出码：0 成功，1 前端/语义/参数/执行或文件输出错误，2 命令格式错误。

H2 自身有错时可导出部分类型报告；参数环境有错时报告列出相应错误。预处理、Parser、H1 失败会阻止 H2。eval 的任一前序或执行阶段失败均不替换旧输出；所有命令继续阻止覆盖主文件和已加载头文件。

## 7. 阅读源码的顺序

| 文件 | 职责 |
|---|---|
| include/vagrad/frontend/hir/typed.hpp | Type、Expr、Stmt、ParamInfo、Probe、HirModule |
| src/frontend/hir/lower.cpp | AST + H1 → 类型化 HIR、语义和能力检查 |
| src/frontend/hir/typed.cpp | 数字解码、BuiltinRegistry、来源诊断、TXT 导出 |
| include/vagrad/frontend/hir/eval.hpp | ParamEnv、EvalInputs、EvalResult 及调用接口 |
| src/frontend/hir/eval.cpp | 参数依赖求值、范围检查、结构化 HIR 参考执行 |
| tools/vagrad_main.cpp | 共用前端链、--typed 与 eval 命令接入 |
| tests/hir_typed_tests.cpp | H2 结构、类型、参数、端点及来源验收 |
| tests/hir_eval_tests.cpp | H3 手算参考与执行语义验收 |
| tests/cli_hir_eval_modes.cmake | 实际 CLI 及输出保护验收 |

C++ 调用核心如下，假定 parsed 已检查成功：

```cpp
auto bindings = std::make_shared<const vagrad::hir::BindingResult>(
    vagrad::hir::bind_names(vagrad::build_syntax_tree(parsed)));
auto module = vagrad::hir::lower_hir(bindings);
if (module.has_errors()) { /* 输出 module.diagnostics 并停止 */ }
else {
    auto parameters = vagrad::hir::resolve_parameters(module, {{"G0", 0.002}});
    vagrad::hir::EvalInputs inputs;
    inputs.node_voltages = {{"d", 0.05}, {"g", 1.0}, {"s", 0.0}};
    inputs.temperature_kelvin = 300.0;
    auto result = vagrad::hir::evaluate_hir(module, parameters, inputs);
    // 先检查 parameters.has_errors() / result.has_errors()，再读取结果。
}
```

ParamEnv 保留其 BindingResult 身份，不能直接用于另一份模型。HirModule 及其来源共享快照，可以在原局部 SourceManager 退出作用域后继续查询。

## 8. 当前执行范围与下一步

本次交付限单 module、一个普通 analog 块、静态 electrical 电位读取与 flow 贡献、标量参数和变量、Block/If/Assign/表达式语句。默认 simple_mos 和 SOFT_THRESHOLD / TRACE_MOS 配置均验收通过。

Parser / AST View 能表达的语法多于 H2/H3 当前执行能力。循环、case、事件、analog initial、用户模拟函数、数组、声明初始化、用户 nature/discipline 访问函数、状态模拟运算和完整电路求解尚不在本执行基线内；遇到会报告能力边界，不能据此宣称完整 BSIM-BULK 已可执行。

后续 M1/C1 和 C2/S1 已接入，见 [mir.md](E:/OpenVAF/VA-Gradient/docs/mir.md) 及 [ssa.md](E:/OpenVAF/VA-Gradient/docs/ssa.md)。HIR、MIR-Local、MIR-SSA 已做数值对齐；下一步为依赖切片与 AD。
