# VA-Gradient 阶段说明

本阶段已打通 **Verilog-A 源码 → 预处理 → 语法分析 → CST → AST View → HIR → MIR-Local → MIR-SSA → 目标切片 → 前向 AD**。可输出变量/支路贡献及其参数导数，支持稀疏导数存储与默认参数依赖链。

| 项目 | 当前状态 |
|---|---|
| 阶段 | 源码前端、语义与静态标量参考求值基线 |
| 状态 | 前端、HIR、MIR/CFG、SSA、目标切片与前向 AD 执行基线完成 |
| 说明日期 | 2026-11-19 |
| 工程版本 | 0.1.0（CMake 工程版本） |
| 实现语言 | C++20 |
| 已验证环境 | Windows x64、Visual Studio 2022 Build Tools、CMake |
| 工程路径 | `E:\OpenVAF\VA-Gradient` |
| 最近一次完整验证 | 53 项 CTest 全部通过，含 C11/DLL、C ABI、ctypes 与 NumPy 数组接口 |

“基线完成”的验收对象是当前增强版 `simple_mos.va`、替代宏配置及错误和边界测试。完整 BSIM-BULK 的全前端语法验收仍属于后续工作。

## 1. 阶段目标与交付

本阶段为后续模型方程分析建立统一的源码入口：先把文本转换为 token，处理宏和 include，随后识别语法、构建保留位置的树，提供类型化字段并建立名称与定义的关联。

| 模块 | 输入 → 输出 | 已完成的主要职责 |
|---|---|---|
| SourceManager / Diagnostic | 文件 → 只读快照、位置、诊断 | 路径去重、FileId、行列索引、原始范围和关联位置 |
| Lexer | 单文件文本 → 原始 Token | 分类名称、数字、字符串、符号等，保留空白、注释、BOM 和续行 |
| Preprocessor | 原始 Token → 展开 Token 与来源图 | 按源码顺序处理 include、宏、条件编译和包含保护 |
| Parser | 展开 Token → ParseEvent 与诊断 | 递归下降、Pratt 表达式、优先级与结合方向、错误恢复 |
| TreeBuilder / CST | ParseEvent → 只读语法树 | 解析父节点链接，补回 trivia，检查 token 覆盖与事件平衡 |
| AST View | CST → 类型化访问接口 | 读取模块、声明、默认值、表达式操作数、分支及循环字段 |
| HIR H1 | AST View → 作用域、符号和引用 | 端口声明合并、名称绑定、读写/调用角色及定位诊断 |
| HIR H2 | AST View + H1 → 类型化 HIR | 数值类型、参数表和范围、Builtin、Probe、Contribution |
| HIR H3 | HIR + 有效参数 + 输入 → 数值结果 | 结构化参考执行、出口变量、贡献和及定位错误 |
| MIR M1 / CFG C1 | HIR → 指令、局部槽、基本块与图 | 降低、校验、导出、基本块执行及 HIR 数值对齐 |
| SSA C2 / S1 | MIR-Local → MIR-SSA | 支配分析、活跃性、phi、重命名、校验和数值对齐 |
| 目标切片 Q1 | SSA + 目标 → 可执行切片 | 数据/控制依赖、输入清单、效果策略和数值对齐 |
| 前向 AD | SSA/切片 + 参数种子 → 原值与雅可比 | 实际路径传播、并行 phi、flow、稀疏存储、参数默认链 |
| CLI / CTest | 命令与样例 → 可复现结果 | 各阶段输出、宏配置传递、错误退出和输入覆盖保护 |

本项目独立实现 C++ 前端与 AD，并提供 C0 代码生成及 Python ctypes 调用。当前交付不包含 OpenVAF 后端、OSDI 生成或 ngspice 仿真。

## 2. 整体工作流程

```text
主 .va 文件
    ↓
SourceManager：加载不可变源码快照
    ↓
Lexer：原始 Token，保留原始位置与 trivia
    ↓
Preprocessor：宏、include、条件编译、来源映射
    ↓
展开 Token
    ↓
Parser：声明/语句递归下降 + Pratt 表达式
    ↓
ParseEvent：StartNode / Token / FinishNode / Error
    ↓
TreeBuilder：补回空白与注释，恢复节点层次
    ↓
CST：只读 arena、NodeId、完整展开文本
    ↓
AST View：模块、参数、表达式与语句的类型化字段
    ↓
HIR H1：Scope / Symbol / Reference，绑定名称并记录来源
    ↓
HIR H2：有类型的 Expr / Stmt，参数、探针与贡献语义
    ├── HIR H3：参考执行 → 出口观察值与贡献
    └── MIR-Local：指令 + 局部槽 + 基本块
            ├── CFG：前驱/后继/可达性 + 校验
            ├── MIR 执行：与 H3 数值对齐
            └── MIR-SSA：支配分析 + phi + 重命名 + SSA 校验/执行
                    ├── 目标切片：保留 CFG，筛选数据与控制依赖
                    └── 前向 AD：种子支持集 + 稀疏存储 + 路径导数
```

Preprocessor 遇到生效的 include 时，会加载头文件并调用同一个 Lexer，再继续处理头文件的 token。include、宏表和条件分支由同一预处理流程共同管理。

终端的 `lex` 命令只扫描当前文件。`preprocess`、`parse` 和 `ast` 会在程序内部调用它们所需的前序模块，运行后续命令不要求先手工运行前面的命令。

### 数据在内存中的衔接

```text
LexResult
    ↓
PreprocessResult
    ├── 文件快照
    ├── ExpandedToken
    ├── ExpansionContext
    └── Diagnostic
    ↓
ParseResult：共享 PreprocessResult + 事件 + 语法诊断
    ↓
SyntaxTree：共享输入 + 只读节点数组
    ↓
ast::Node / ast::Token：共享树存储的类型化句柄
    ↓
hir::BindingResult：共享 CST + 作用域 + 定义 + 引用 + 诊断
    ↓
hir::HirModule：类型化表达式 / 语句 + ParamInfo + Probe
    ├── HIR 执行 + ParamEnv + EvalInputs → EvalResult
    └── mir::MirFunction → CFG / 校验 / MIR 执行 → EvalResult
```

TXT 是供查看的导出报告。程序各阶段通过内存对象衔接，不通过读取这些报告传递数据。AST View 也不构建第二棵树。

## 3. 关键设计约定

### 原始位置与展开位置分别保存

- `SourceSpan` 表示单一原始文件中的字节范围，采用左闭右开区间 `[begin,end)`。
- `Expanded[a,b)` 表示展开文本中的范围，可能对应主文件、头文件或宏体中的文字。
- 行号和字节列号从 1 开始；字节偏移从 0 开始。
- FileId、NodeId 和上下文编号属于本次运行及所属存储，不能跨运行假设编号不变。

宏展开上下文记录调用位置、定义位置及定义时的上下文；实参上下文还记录替换前的来源。因此，同一原始实参被复制多次后，可以保留共同的原始位置，同时具有不同的展开 token 和语法节点身份。

### 语法识别与构树分离

Parser 消费有效记号并产生事件。Pratt 使用 `forward_parent` 链接稍后确认的父表达式，TreeBuilder 再按正确层次打开节点。

Parser 判断语法时跳过空白和注释，TreeBuilder 按 token 索引补回。文件末尾的 trivia 通过 EOF 事件保留，CST 根文本可以逐字节重建展开后的代码。

### 类型化访问与语义分析分层

AST View 已提供以下接口：

```cpp
module.parameters();          // 模块头和模块体中的参数声明
declaration.declarators();    // 一条声明中的各个名字
declarator.initializer();     // 默认值或初始化表达式
assignment.lhs();             // 赋值左侧
assignment.rhs();             // 赋值右侧
if_stmt.condition();          // if 条件
call.arguments();             // 调用实参
```

这里的“类型化”指把节点解释为模块、参数、赋值或表达式等语法类别。H1 已在这些 View 上完成名字绑定；表达式类型推导、单位和数值尚未计算。

可选或缺失字段使用 `std::optional`；Parser 已创建的 ErrorNode 可以作为 Expr/Statement 占位返回。整体是否有错误应检查阶段诊断，不能仅凭是否存在 ErrorNode 判断。

### H1 名称绑定

H1 分两遍处理：先收集声明与作用域，再关联引用。模块端口头、方向和 discipline 声明完善同一符号；命名块和函数局部定义优先于外层定义。普通 begin/end 不额外创建词法作用域。

默认样例得到 4 个作用域、18 个用户符号和 39 处已解析引用，H1 错误为零。输出记录名称是 Read、Write、Call、Type 还是端点引用，并保留 SymbolId 与宏来源。完整说明见 [HIR H1](E:/OpenVAF/VA-Gradient/docs/hir-h1.md)。

### H2 / H3 语义与参考执行

H2 为表达式建立类型，参数使用处保留 Ref，不提前替换为默认常量；V(g,s) 转为有方向的电位读取，I(ds)<+... 转为独立 flow 贡献。H3 直接执行结构化 HIR，并区分 ids@exit 与 flow(ds)@exit。

现有 simple_mos 的六组手算参考全部通过，包含截止、线性、饱和、边界、禁用和温度变化。使用方法、代码入口与执行范围见 [H2/H3 说明](E:/OpenVAF/VA-Gradient/docs/hir-h2-h3.md)。

### MIR-Local 与 CFG

MIR 将 HIR 表达式降低为带 ValueId 的指令，源码变量暂保留为 Local 槽。if、&&、|| 和三目表达式拆为基本块，CFG 从终结指令派生。校验器检查编号、类型、单一定义及定义对使用的支配关系。

MIR-Local 默认样例生成 10 个块、6 个局部槽和 47 个值。独立基本块执行器已通过手算点、偏置网格及 HIR 对齐。详见 [MIR 说明](E:/OpenVAF/VA-Gradient/docs/mir.md)。

### SSA 与 phi

已实现支配树、支配边界、活跃性裁剪、phi 插入及沿支配树重命名。SSA 中移除所有 Local Load/Store，Return 显式记录变量出口值；未赋值检查和诊断效果仍保留。

默认样例 SSA 为 10 个块、42 个值和 3 个 phi，数值与 HIR/MIR-Local 对齐。phi 输入在对应前驱边上校验，执行时按前驱同时更新。详见 [SSA 说明](E:/OpenVAF/VA-Gradient/docs/ssa.md)。

## 4. 目录与源码入口

以下目录均位于 `E:\OpenVAF\VA-Gradient`。

```text
VA-Gradient/
├── CMakeLists.txt / CMakePresets.json  构建目标与 Windows 配置
├── include/vagrad/
│   ├── common/                       源码快照、范围、诊断接口
│   ├── frontend/
│       ├── lexer/                    Token 和 Lexer
│       ├── preprocessor/             宏、include、展开结果与来源
│       ├── parser/                   ParseEvent 和 Parser
│       ├── syntax/                   SyntaxKind、NodeId、CST
│       ├── ast/                      AST View 公共接口
│       └── hir/                      H1 绑定、H2 类型化 IR、H3 执行接口
│   ├── ir/mir/                       MIR-Local、CFG、校验与执行接口
│   ├── analysis/                     支配集合、支配树与支配边界
│   └── runtime/                      HIR/MIR 共用标量运算
├── src/                              与 include 对应的实现目录
├── tools/vagrad_main.cpp             CLI 入口
├── tests/                            库测试与 CLI 集成测试
├── testdata/
│   ├── simple_mos.va                 正常验收样例
│   ├── include/simple_mos_defaults.vams
│   ├── parser_errors.va              故意含错误的恢复样例
│   └── semantic/                    名称/类型/运行错误与参数依赖样例
├── scripts/build.ps1                 本机构建与测试脚本
├── docs/                             各模块实现说明和进度记录
└── build/                            编译、测试及导出结果
```

| 阅读目的 | 主要入口 |
|---|---|
| 源码快照与位置 | [source_manager.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/common/source_manager.hpp) |
| 单文件词法分析 | [lexer.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/lexer/lexer.hpp) |
| 预处理与来源结构 | [preprocessor.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/preprocessor/preprocessor.hpp) |
| Parser 与事件 | [parser.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/parser/parser.hpp)、[parse_event.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/parser/parse_event.hpp) |
| 只读 CST 与构树 | [syntax_tree.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/syntax/syntax_tree.hpp) |
| 类型化字段接口 | [ast.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/ast/ast.hpp) |
| H1 定义与引用接口 | [hir.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/hir/hir.hpp) |
| H2 类型化 HIR | [typed.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/hir/typed.hpp) |
| H3 参数与执行接口 | [eval.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/frontend/hir/eval.hpp) |
| MIR / CFG 接口 | [mir.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/ir/mir/mir.hpp) |
| MIR 执行接口 | [MIR eval.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/ir/mir/eval.hpp) |
| SSA 与支配分析 | [dominance.hpp](E:/OpenVAF/VA-Gradient/include/vagrad/analysis/dominance.hpp)、[ssa.cpp](E:/OpenVAF/VA-Gradient/src/ir/mir/ssa.cpp) |
| CLI 调用链 | [vagrad_main.cpp](E:/OpenVAF/VA-Gradient/tools/vagrad_main.cpp) |

正式代码、测试和样例分别保存；编译与导出文件统一进入 build。生成报告可以通过命令重建，后续代码更新沿现有模块目录维护。

## 5. 构建与复现

### 环境

工程要求 C++20 和 CMake 3.21 或以上。当前验证使用 Windows x64 与 MSVC，CMake 需要在 PATH 中。

构建脚本默认使用：

```text
E:\OpenVAF\tools\vs-buildtools\VC\Auxiliary\Build\vcvars64.bat
```

若 Build Tools 位于其他目录，可向脚本传入 `-BuildToolsRoot`；`-SkipTests` 表示只构建。脚本会加载开发环境，执行 CMake 配置、构建及 CTest。

### 构建与快速查看

在 PowerShell 中运行。后续命令共用以下变量：

```powershell
$vaGradientRoot = 'E:\OpenVAF\VA-Gradient'
$vaGradientExe = Join-Path $vaGradientRoot 'build\windows-msvc\vagrad.exe'
$vaGradientSample = Join-Path $vaGradientRoot 'testdata\simple_mos.va'
$vaGradientOutput = Join-Path $vaGradientRoot 'build'

Set-Location -LiteralPath $vaGradientRoot
& (Join-Path $vaGradientRoot 'scripts\build.ps1')

# 一条命令自动完成前端流程并导出 AST View。
& $vaGradientExe ast $vaGradientSample -o (Join-Path $vaGradientOutput 'simple_mos.ast.txt')

# 在同一条前序链上执行 H1 名称绑定。
& $vaGradientExe hir $vaGradientSample -o (Join-Path $vaGradientOutput 'simple_mos.hir.txt')

# H2 类型化 HIR、H3 参考执行。
& $vaGradientExe hir $vaGradientSample --typed -o (Join-Path $vaGradientOutput 'simple_mos.typed.hir.txt')
& $vaGradientExe eval $vaGradientSample --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o (Join-Path $vaGradientOutput 'simple_mos.eval.txt')

# MIR-Local、CFG、MIR 执行。
& $vaGradientExe mir $vaGradientSample --form local -o (Join-Path $vaGradientOutput 'simple_mos.mir.txt')
& $vaGradientExe cfg $vaGradientSample -o (Join-Path $vaGradientOutput 'simple_mos.cfg.txt')
& $vaGradientExe eval $vaGradientSample --ir mir-local --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o (Join-Path $vaGradientOutput 'simple_mos.mir.eval.txt')

# SSA、支配分析与 SSA 执行。
& $vaGradientExe mir $vaGradientSample --form ssa -o (Join-Path $vaGradientOutput 'simple_mos.ssa.txt')
& $vaGradientExe cfg $vaGradientSample --dominators -o (Join-Path $vaGradientOutput 'simple_mos.dominance.txt')
& $vaGradientExe eval $vaGradientSample --ir mir-ssa --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o (Join-Path $vaGradientOutput 'simple_mos.ssa.eval.txt')

```

### 分阶段复现输出

```powershell
# 查看输入信息、字节位置和主文件原始 token。
& $vaGradientExe source $vaGradientSample
& $vaGradientExe locate $vaGradientSample 0
& $vaGradientExe lex $vaGradientSample

# 展开后的 VA 文本，以及主文件与头文件统一的 token 报告。
& $vaGradientExe preprocess $vaGradientSample -o (Join-Path $vaGradientOutput 'simple_mos.expanded.va')
& $vaGradientExe preprocess $vaGradientSample --dump-tokens -o (Join-Path $vaGradientOutput 'simple_mos.tokens.txt')

# 语法事件与 CST。
& $vaGradientExe parse $vaGradientSample --dump-events -o (Join-Path $vaGradientOutput 'simple_mos.events.txt')
& $vaGradientExe parse $vaGradientSample -o (Join-Path $vaGradientOutput 'simple_mos.cst.txt')

# 类型化语法视图。
& $vaGradientExe ast $vaGradientSample -o (Join-Path $vaGradientOutput 'simple_mos.ast.txt')

# 作用域、符号与引用绑定报告。
& $vaGradientExe hir $vaGradientSample -o (Join-Path $vaGradientOutput 'simple_mos.hir.txt')
```

| 命令 | 默认行为 |
|---|---|
| `source` | 显示文件路径、FileId、字节数和行索引数量 |
| `locate` | 把字节偏移转换为原始行列 |
| `lex` | 单文件原始 token，不读取 include |
| `preprocess` | 展开后的源码；加 `--dump-tokens` 输出 token 与来源表 |
| `parse` | CST；加 `--dump-events` 输出事件序列 |
| `ast` | 类型化语法视图 |
| `hir` | H1 作用域、定义、引用及诊断；`--typed` 进入 H2 并求参数有效值 |
| `mir` | 默认 MIR-Local；--form ssa 构造、校验并导出 SSA |
| `cfg` | CFG；--dominators 追加 idom、支配树子节点和支配边界 |
| `slice` | --target ids@exit 或 flow(branch)@exit；效果策略 preserve / target-only |
| `eval` | --ir hir / mir-local / mir-ssa / mir-slice；用 --param、--voltage、--temperature 提供输入 |

不指定 `-o` 时输出到终端。预处理、解析及 AST 命令共用 `-D` 和 `-I` 配置；阶段专属的输出选项不能混用。

### 改变宏配置与头文件搜索目录

```powershell
# 选择 SOFT_THRESHOLD 分支，并保留 TRACE_MOS 的 $strobe。
& $vaGradientExe ast $vaGradientSample -D SOFT_THRESHOLD -D TRACE_MOS

# 增加 include 搜索目录。
& $vaGradientExe preprocess $vaGradientSample -I (Join-Path $vaGradientRoot 'testdata\include')
```

`-D NAME` 默认定义为 1；`-D NAME=VALUE` 指定替换文字。`ifdef` 判断名字是否已定义，因此 `-D NAME=0` 仍属于已定义。include 优先相对于当前文件查找，再依次使用 `-I` 目录。

## 6. 验收样例与实际结果

### 正常样例

[simple_mos.va](E:/OpenVAF/VA-Gradient/testdata/simple_mos.va) 包含中文注释、块注释、头文件、多行宏、嵌套宏、条件编译、属性、参数区间、分支和贡献语句。默认参数位于 [simple_mos_defaults.vams](E:/OpenVAF/VA-Gradient/testdata/include/simple_mos_defaults.vams)。

默认配置下：

```verilog
parameter real G0 = 1m from (0:inf);
parameter real VTH0 = 0.45 from [0:inf);
vov = max((((vgs) - (VTH0))), 0.0);
```

SOFT_THRESHOLD 配置选择带 sqrt 的分支；TRACE_MOS 配置保留调试调用。两种配置均已通过预处理、Parser 和 AST View 测试。

以下统计对应当前默认样例；修改样例后应以重新运行的结果为准。

| 指标 | 结果 |
|---|---:|
| 主文件 / 头文件字节数 | 2,018 / 207 |
| 主文件原始 token | 579 |
| 展开文本字节数 | 1,713 |
| 展开 token，含 trivia 和 EOF | 486 |
| 来源上下文 | 17 |
| ParseEvent 总数 | 574 |
| StartNode / Token / FinishNode / Error | 159 / 256 / 159 / 0 |
| CST 节点 / token 叶子 | 159 / 486 |
| AST View 节点记录 | 159 |
| 模块 / 参数声明 / 赋值 / if / 贡献语句 | 1 / 7 / 7 / 2 / 1 |
| H1 作用域 / 用户符号 / 引用 / 成功绑定 / 错误 | 4 / 18 / 39 / 39 / 0 |

Token 事件数少于 CST token 叶子数，是因为空白和注释由 TreeBuilder 补回。AST View 使用已有 CST 节点，因此节点记录数量相同。

### 输出文件

| 文件 | 主要用途 |
|---|---|
| [simple_mos.expanded.va](E:/OpenVAF/VA-Gradient/build/simple_mos.expanded.va) | 直接阅读展开后的公式 |
| [simple_mos.tokens.txt](E:/OpenVAF/VA-Gradient/build/simple_mos.tokens.txt) | 检查 token、展开范围及原始来源 |
| [simple_mos.events.txt](E:/OpenVAF/VA-Gradient/build/simple_mos.events.txt) | 检查 Parser 的实际事件及父节点链接 |
| [simple_mos.cst.txt](E:/OpenVAF/VA-Gradient/build/simple_mos.cst.txt) | 检查完整层次、空白、注释和 token |
| [simple_mos.ast.txt](E:/OpenVAF/VA-Gradient/build/simple_mos.ast.txt) | 阅读名称、类型文字、默认值、操作数和分支角色 |
| [simple_mos.hir.txt](E:/OpenVAF/VA-Gradient/build/simple_mos.hir.txt) | 阅读作用域、定义、声明合并、引用目标和诊断 |

例如，AST View 中的 G0 默认值仍能定位到头文件宏定义，而不是只保留一个脱离来源的 `1m` 字符串。

### 错误恢复样例

[parser_errors.va](E:/OpenVAF/VA-Gradient/testdata/parser_errors.va) 故意包含“缺少表达式”和“缺少分号”两处错误：

```powershell
$vaGradientErrorSample = Join-Path $vaGradientRoot 'testdata\parser_errors.va'
& $vaGradientExe ast $vaGradientErrorSample
$LASTEXITCODE
```

该命令预期返回 1，报告两处诊断，并继续保留后续正确赋值及贡献语句。语法错误可以导出部分事件或树，诊断同时写入标准错误流；预处理失败时，parse/ast 不进入语法阶段。

CLI 成功返回 0，处理或输出错误返回 1，命令格式错误返回 2。输出路径会检查本次实际加载的输入，防止覆盖主文件或头文件。

[binding_errors.va](E:/OpenVAF/VA-Gradient/testdata/semantic/binding_errors.va) 用于 H1 验收：语法正确，但有重复 G0、未定义变量和未定义函数三处问题。hir 会保留可查看的部分绑定报告并返回 1；上游语法有错时则不进入 H1。

## 7. 验证与验收证据

最近一次完整 CTest 记录为 **53/53 通过**。含模型梯度、导数 IR、默认链、稀疏/密集、核验、C/DLL、ctypes 和 NumPy；绑定测试按本机可选依赖注册。

| 测试组 | 验证重点 |
|---|---|
| common | 混合换行、位置边界、路径去重、文件快照寿命和诊断 |
| lexer | 分类、数值形式、转义、错误恢复及逐字节原文重建 |
| preprocessor | 宏与实参、条件分支、嵌套/重复 include、包含保护、来源图和错误诊断 |
| parser | 优先级与结合方向、事件完整性、错误后继续解析、CST 无损重建、长表达式及递归限制 |
| ast | 字段角色、作用域、缺失字段、宏来源、实参复制身份、只读存储和 View/Token 生命周期 |
| hir | 声明合并、作用域与遮蔽、函数参数/返回变量、读写角色、未定义名、宏来源及生命周期 |
| hir_typed | 类型、转换、参数依赖/覆盖/范围、探针方向、来源和能力诊断 |
| hir_eval | 六组独立手算参考、短路、贡献累加、诊断记录与执行错误 |
| mir / mir_verify | 降低、CFG、指令编号和类型、定义支配使用、畸形图及回边 |
| mir_eval | 六组手算点、偏置网格、短路与诊断、HIR/MIR 数值及错误对齐 |
| ssa | 支配分析、phi/重命名、三种形式对齐、未赋值、循环并行 phi 与畸形 SSA |
| slice | 目标输入、控制依赖、效果策略、投影、循环与数值对齐 |
| ad_primitives / ad_rules | 链式规则、手算与中心差分、域/边界策略、稳定公式 |
| ad_integration / ad_parameters | 偏置/温度网格、循环 phi、64 种子、稀疏/密集与默认参数链 |
| ad_ir / ad_prepared | 导数局部 DAG、折叠顺序、数学规则、快照和批量状态隔离 |
| example_ad_batch | 正式示例从源码解析一次，运行多组偏置 |
| gradcheck | 同参数模式的差分参考、范围/路径/步长与完成的不匹配 |
| cgen_mos / cgen_math / cgen_control | 生成 C11 后编译成 DLL，与 C++ 逐点对齐，含未赋值和批量错误隔离 |
| python_kernel | ctypes 单点/批量、参数元信息、localparam 和错误处理 |
| python_numpy | 数组形状/布局、原值/雅可比、直接入参、显式复制、输出复用和失败隔离 |
| 27 项 CLI 测试 | 命令衔接、配置传递、输出、退出码与输入覆盖保护 |

已核对的关键约束：

- 每枚有效展开 token 都有顺序一致的 Token 事件，trivia 由构树器补回。
- CST 中每枚输入 token 按原顺序出现一次，根文本与展开代码逐字节一致。
- 新增 AST View 后，原有事件和 CST 导出内容逐字节不变。
- 复制宏实参可以共享原始位置，同时保留不同的展开 token 和节点身份。
- View 与树共享存储；保留 View 后，即使原来的局部解析对象退出作用域，也能继续访问。
- 未选中分支中的 include 不会被打开；语法错误不会静默丢掉恢复区段的源码。
- H1 的引用绑定到明确符号或带诊断保留；宏复制的不同引用出现可以指向同一 SymbolId。

可在项目目录下仅重跑现有测试：

```powershell
Set-Location -LiteralPath $vaGradientRoot
ctest --preset windows-msvc
```

详细运行记录位于 [LastTest.log](E:/OpenVAF/VA-Gradient/build/windows-msvc/Testing/Temporary/LastTest.log)，重新运行 CTest 会更新该记录。

## 8. 当前能力边界

1. **语言覆盖为前端基线子集。** 已覆盖当前示例需要的声明、属性、参数范围、模拟块与函数、主要表达式、分支、循环和事件语句。完整数字 Verilog、generate、模块实例化、全部数组初始化等尚未覆盖。
2. **预处理仍有明确范围。** 当前不支持 token 粘接、宏字符串构造、默认宏实参、预定义 `__FILE__` / `__LINE__` 及部分编译器指令；条件块在单文件内配对，超限嵌套会产生诊断。
3. **完整模型尚未验收。** 先前对 BSIM-BULK 做过单文件 Lexer 的逐字节重建验证；这不等于完整模型已通过预处理、Parser 和 AST View。
4. **HIR/MIR/SSA 为静态标量基线。** 支持单模块、单 analog 块、Block/If/Assign、参数、纯数学、电位读取和 flow 贡献。VA 循环、事件、用户模拟函数、状态模拟运算仍未接通；后端循环通过人工 MIR 验证。SSA、切片和参数前向 AD 已实现；量纲检查、完整整数位宽、层次/成员路径仍未支持。AD 执行器输出数值梯度，尚不生成带导数的新 MIR 或 C/OSDI 内核。
5. **CST 为只读 arena 基线。** 当前未实现增量编辑、节点驻留去重或完整 red/green 双层树。未生效的条件分支保留在原始快照中，不作为展开 CST 的节点。

更细的支持范围以各模块说明为准。本阶段的完成判据是前端链路可运行、结构与来源可检查、结果可复现，为后续语义分析提供稳定入口。

## 9. 后续实施顺序

H1/H2/H3、M1/C1、C2/S1、Q1、AD、导数 IR、快照、C0、ctypes 与 NumPy 基线已完成。后续扩大模型覆盖，推进 SIMD、工作区复用与产物缓存。

| 后续阶段 | 主要工作 | 预期验收 |
|---|---|---|
| H1：名称绑定（已完成） | 作用域、符号、端口声明合并、引用关联 | 当前默认与替代配置已通过 |
| H2/H3：语义与参考求值（基线已完成） | 类型、有效参数、内建函数、探针、贡献与 HIR 解释器 | ids 和贡献值已与六组手算用例对齐 |
| M1/C1：MIR 与 CFG（基线已完成） | 指令、可变局部槽、基本块、跳转、图校验与执行 | MIR-Local 与 HIR 已逐点对齐 |
| C2/S1：SSA（基线已完成） | 支配分析、phi、重命名、SSA 校验 | SSA 结果已与前两种执行形式对齐 |
| Q1：目标切片（基线已完成） | 出口值、控制流切片、效果策略与输入清单 | 切片与原 SSA 目标值已对齐 |
| 前向 AD（执行基线已完成） | 实数参数种子、稀疏支持、phi/flow 导数、默认依赖链 | 手算、差分及密集参考逐点对齐 |
| 向量导数 IR / C++ 批量（基线完成） | 导数表达式 DAG、校验、导出、只读执行快照 | 算术原语、模型差分和批量隔离对齐 |
| C0 / ctypes（基线完成） | 自包含 C11、单点/批量 ABI、元信息、动态库、Python 封装 | 三组 C 样例及 Python 测试通过 |
| NumPy（基线完成） | 合规数组直接传入、雅可比张量、输出复用与布局校验 | 数值、布局及失败状态已验收 |
| 性能扩展 | SIMD、工作区复用、产物缓存 | 后续工作 |
| 真实模型扩展 | 扩大语法和执行语义覆盖 | 后续工作 |

CFG 是 MIR 基本块与跳转关系的派生图，SSA 构造依赖 CFG。完整方案见 [HIR、MIR 与 CFG 实施方案](E:/OpenVAF/VA-Gradient/docs/hir-mir-cfg-plan.md)。前向 AD 以校验后的 SSA 或切片为输入，沿同一 CFG 路径传播。

## 10. 详细说明索引

- [预处理器：宏、include 与来源映射](E:/OpenVAF/VA-Gradient/docs/preprocessor.md)
- [Parser、ParseEvent 与 CST](E:/OpenVAF/VA-Gradient/docs/parser.md)
- [AST View：接口、错误字段与生命周期](E:/OpenVAF/VA-Gradient/docs/ast-view.md)
- [HIR H1：作用域、符号表与名称绑定](E:/OpenVAF/VA-Gradient/docs/hir-h1.md)
- [HIR H2/H3：类型、参数与参考求值](E:/OpenVAF/VA-Gradient/docs/hir-h2-h3.md)
- [MIR-Local、CFG、校验与数值执行](E:/OpenVAF/VA-Gradient/docs/mir.md)
- [SSA、phi、支配分析与重命名](E:/OpenVAF/VA-Gradient/docs/ssa.md)
- [目标切片与效果策略](E:/OpenVAF/VA-Gradient/docs/slice.md)
- [前向 AD：参数模式、稀疏存储与梯度输出](E:/OpenVAF/VA-Gradient/docs/ad.md)
- [梯度核验：中心差分矩阵与报告](E:/OpenVAF/VA-Gradient/docs/gradcheck.md)
- [导数 IR：表达式、校验与实际执行](E:/OpenVAF/VA-Gradient/docs/ad-ir.md)
- [编译一次、多点求导与 C++ 示例](E:/OpenVAF/VA-Gradient/docs/ad-prepared.md)
- [C0 生成、动态库、批量 C ABI 与 Python](E:/OpenVAF/VA-Gradient/docs/c-codegen.md)
- [NumPy 批量原值和雅可比](E:/OpenVAF/VA-Gradient/docs/numpy.md)
- [分阶段开发记录与接口约定](E:/OpenVAF/VA-Gradient/docs/development.md)
- [HIR、MIR、CFG 后续实施方案](E:/OpenVAF/VA-Gradient/docs/hir-mir-cfg-plan.md)


## Q1 目标切片

已支持从 SSA 的变量出口或命名支路 flow 构造可执行切片。默认 preserve 保留原执行检查与副作用；显式 target-only 用于隔离目标计算。simple_mos 的 ids 切片为 32/57 条指令，flow(ds) 为 37/57 条，CFG 保持保留。详细说明与命令见 [slice.md](E:/OpenVAF/VA-Gradient/docs/slice.md)。

## 前向 AD 执行基线

已接通模型级 SSA 原值/导数同步执行、flow 导数、稀疏存储和 CLI。默认对独立有效参数求偏导；可用 --parameter-derivatives defaults 传播非覆盖参数的默认依赖链。非光滑边界采用保守诊断。详细说明见 [ad.md](E:/OpenVAF/VA-Gradient/docs/ad.md)，当前恢复入口见 [ad-checkpoint.md](E:/OpenVAF/VA-Gradient/docs/ad-checkpoint.md)。

```powershell
Set-Location -LiteralPath 'E:\OpenVAF\VA-Gradient'
.\build\windows-msvc\vagrad.exe eval .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 -o .\build\simple_mos.ad.txt
.\build\windows-msvc\vagrad.exe ad-plan .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM -o .\build\simple_mos.ad-plan.txt
```

在上述偏置下 d(ids)/d(G0)=0.02625、d(ids)/d(VTH0)=-5e-5；d(flow:ds)/d(TNOM)=8.75e-8。结果是给定端电位下的模型参数导数；当前不执行电路工作点求解。

可将 eval 换为 gradcheck，并添加 --step G0=1e-7 等选项，生成 AD/中心差分逐项核验报告，详见 gradcheck.md。核验未完成时保留旧报告；计算完成但不匹配时可导出完整 FAIL 报告。

ad-plan 同时导出向量导数 IR。examples/forward_ad_demo.cpp 展示 Prepared 的 C++ 顺序批量；emit-c 则另外生成独立 C11 原值/梯度内核，带单点/批量 ABI。二者的接口和能力范围分别见 ad-prepared.md、c-codegen.md。

```powershell
.\build\windows-msvc\vagrad.exe emit-c .\testdata\simple_mos.va --wrt G0 --wrt VTH0 --wrt TNOM -o .\build\my_model.c
python .\python\vagrad_kernel.py .\build\windows-msvc\vagrad_cgen_mos.dll --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300
```

C0 仅支持独立有效参数模式与明确的标量子集；C++ AD 另支持 defaults 链传播。NumPy 合规入参可直接传给 C，输出与内部工作区仍需存储；尚无 SIMD、OSDI 或完整模型支持。
