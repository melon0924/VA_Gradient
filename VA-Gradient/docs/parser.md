# Parser、ParseEvent 和 CST 基线

## 1. 当前构建结果

项目现在可以运行下列完整流程：

```text
.va 文件
  → 已有 Lexer：单文件原始 token
  → 已有 Preprocessor：include、宏、条件编译、来源映射
  → 新增 Parser：识别语法，发出 ParseEvent，记录语法错误
  → 新增 TreeBuilder：消费事件，补回 trivia，构造只读 CST
```

Parser 直接使用内存中的 `PreprocessResult`。没有另写 Lexer 或 include 处理器，也不读取 CLI 保存的 token 报告作为输入。

默认 `simple_mos.va` 已实际运行，结果如下：

| 项目 | 结果 |
|---|---:|
| 展开 token，含空白、注释及 EOF | 486 |
| 来源上下文 | 17 |
| StartNode 事件 | 159 |
| Token 事件，含 EOF | 256 |
| FinishNode 事件 | 159 |
| Error 事件 | 0 |
| 事件总数 | 574 |
| CST 节点 | 159 |
| CST token 叶子 | 486 |

`SOFT_THRESHOLD + TRACE_MOS` 配置也通过解析。新增流程没有改变原先预处理输出的任何字节。CST 根节点的文本能够逐字节重建完整展开代码。

## 2. 在终端运行

```powershell
Set-Location 'E:\OpenVAF\VA-Gradient'

# 重新编译，并运行当前全部测试。
.\scripts\build.ps1

# 先预处理，再解析，保存 ParseEvent。
.\build\windows-msvc\vagrad.exe parse .\testdata\simple_mos.va --dump-events -o .\build\simple_mos.events.txt

# 使用同一条解析链，保存 CST。
.\build\windows-msvc\vagrad.exe parse .\testdata\simple_mos.va -o .\build\simple_mos.cst.txt

# 原有预处理配置仍可直接使用。
.\build\windows-msvc\vagrad.exe parse .\testdata\simple_mos.va -D SOFT_THRESHOLD -D TRACE_MOS
```

输出说明：

| 文件 | 用途 |
|---|---|
| `build/simple_mos.expanded.va` | 预处理后的代码，便于直接读公式 |
| `build/simple_mos.tokens.txt` | 预处理 token 与来源映射 |
| `build/simple_mos.events.txt` | Parser 实际生成的线性事件序列 |
| `build/simple_mos.cst.txt` | TreeBuilder 整理后的层次结构 |

这些文件是供查看的导出结果。程序内部依靠 C++ 对象传递数据，运行 parse 不要求先在磁盘生成前两份文件。不指定 `-o` 时直接输出到终端。

成功返回 0，预处理/语法/输出错误返回 1，命令格式错误返回 2。预处理失败时不进入 Parser；语法失败时仍可导出事件或错误 CST，诊断同时显示在标准错误流。

## 3. 直接从 C++ 调用

```cpp
SourceManager sources;
const auto file = sources.load("model.va");

// 1. 复用原来的预处理器。
auto input = std::make_shared<const PreprocessResult>(
    preprocess(sources, file)
);

// 2. 在已有 token 上识别语法，得到事件与诊断。
auto parsed = parse(input);

// 3. 根据事件构造树。
auto tree = build_syntax_tree(parsed);
```

`ParseResult` 包含：

```cpp
std::shared_ptr<const PreprocessResult> input; // 原来的展开 token 与文件快照
std::vector<ParseEvent> events;               // 线性构树事件
std::vector<Diagnostic> diagnostics;          // 本阶段语法错误
```

`has_errors()` 同时考虑预处理错误与语法错误。上面的简化调用展示数据流；实际批处理应先检查 input 的错误，再决定是否解析。正式 CLI 已执行这项检查。

使用 shared_ptr 持有只读输入，是为了让解析结果和树在 SourceManager 退出作用域后仍能访问原始文字。树的叶子继续引用已有展开 token，不复制成另一套不带来源的字符串。

## 4. Parser 如何识别当前模型

`parser.cpp` 使用递归下降识别声明、语句和代码块，使用 Pratt 方法处理表达式。

例如：

```verilog
ids = LEAK * vds;
```

Parser 识别出 AssignmentStmt，左侧为 NameExpr，右侧为 BinaryExpr，末尾为分号。乘法的两个操作数分别是 LEAK 和 vds。

```verilog
ids = a + b * c;
```

Pratt 根据优先级把右侧整理为：

```text
BinaryExpr: +
├── NameExpr: a
└── BinaryExpr: *
    ├── NameExpr: b
    └── NameExpr: c
```

这里仅建立语法关系。a 是否已经声明、类型是否匹配、表达式数值是多少，留给后续符号绑定和语义阶段。

运算符优先级与结合方向参照 [Accellera Verilog-AMS 2.4 LRM 第 4.2.2 节](https://www.accellera.org/images/downloads/standards/v-ams/VAMS-LRM-2-4.pdf)。一元运算优先于幂运算，二元运算符（包括 **）左结合，条件运算符 ?: 右结合。相应结构已有测试。

## 5. ParseEvent 的实际形式

事件类型及其使用字段：

| 类型 | 字段 | 作用 |
|---|---|---|
| StartNode | node_kind、forward_parent | 开始节点；可链接到稍后出现的父节点事件 |
| Token | token_index | 消费指定索引的展开 token |
| FinishNode | 无额外字段 | 结束当前节点 |
| Error | diagnostic_index | 引用本次解析产生的一条诊断 |

Parser 对有效记号及 EOF 发出 Token 事件，判断语法时跳过空白和注释。token_index 是完整展开流中的索引，因此事件中的 token 编号有间隔，这些间隔对应 trivia。

### 为什么有 forward_parent

从左向右读取 `a + b` 时，Parser 先知道 a 是 NameExpr，读到 + 后才知道应在外面包一个 BinaryExpr。`start()` 预留起始事件，`complete()` 填入种类并追加结束事件，`precede()` 则给已经完成的左表达式增加父节点链接。

下面是说明原理的事件示意，并非当前示例文件的精确编号：

```text
Event[10] StartNode(NameExpr) ForwardParent[13]
Event[11] Token("a")
Event[12] FinishNode
Event[13] StartNode(BinaryExpr)
Event[14] Token("+")
Event[15] StartNode(NameExpr)
Event[16] Token("b")
Event[17] FinishNode
Event[18] FinishNode
```

TreeBuilder 读到 Event[10] 时先沿链接找到 Event[13]，依次打开 BinaryExpr 和 NameExpr。随后正常处理 a、+、b。再次到达 Event[13] 时跳过这个已打开的起始事件，避免重复创建父节点。

这允许 Parser 使用追加事件的方式构造表达式，不需要把整段事件反复向后移动。打开节点的逻辑顺序与原始事件的存储顺序因此可能不同；阅读层次关系时以 CST 输出更直观。

## 6. TreeBuilder 与 CST

TreeBuilder 在 `syntax_tree.cpp` 中：

1. 遇到 StartNode 时分配节点，用栈记录当前父节点。
2. 遇到 Token 事件时，补入自上一枚有效 token 之后的空白和注释，再放入指定 token。
3. 遇到 FinishNode 时保存节点的 token 范围并出栈。
4. Error 事件引用诊断；恢复时消耗的错误代码由 ErrorNode 保存。

EOF 事件也会补入文件末尾的空白和注释，因此尾部文字不会丢失。

构建期间会检查：节点是否平衡、父链接是否有效、有效 token 是否遗漏，以及整份输入是否被全部消费。Parser 自身的内部错误会得到异常，正常的用户语法错误则保存在 ParseResult 中。

当前 CST 使用只读 arena 存储：

```text
SyntaxTree
├── 共享的 PreprocessResult
└── nodes 数组
    ├── SyntaxKind
    ├── children：子节点编号或 token 索引
    └── token_begin / token_end
```

`NodeId` 是当前树内的节点编号，通过 `tree.node(id)` 读取节点。`tree.text(id)` 通过 token 范围取得节点的完整展开文字。发布后的节点只能读取，复制 SyntaxTree 共享底层存储。

这是可供后续 AST View 使用的 CST 基础，尚未实现节点驻留去重、增量编辑或完整的 red/green 双层结构。

输出中的 Expanded 范围属于展开文本，File/Context 和后面的原始位置则沿用预处理来源映射。节点范围可能包含归属于它的前导空白和注释，不一定恰好从关键字开始。

## 7. 观察错误恢复

`testdata/parser_errors.va` 故意包含两处错误：

```verilog
ids = vgs + ;       // 缺少表达式
vds = V(d, s);      // 后续正确语句
ids = vgs * vds     // 缺少分号
ids = 0.0;          // 后续正确语句
I(d, s) <+ ids;
```

运行：

```powershell
.\build\windows-msvc\vagrad.exe parse .\testdata\parser_errors.va --dump-events
```

当前会报告：

```text
parser_errors.va:9:21: error[PARSE_EXPRESSION]: expected an expression
parser_errors.va:12:9: error[PARSE_EXPECTED]: expected ';'
```

第二条诊断指向 Parser 本来应该见到分号、实际却见到下一条 ids 的位置。Parser 不会给源码伪造一个分号 token，而是记录错误后继续识别后面的赋值和 ContributionStmt。

恢复通常以分号、块结束符或下一条明确的声明/赋值为边界。输入中的错误 token 仍会被保留。宏体内的语法错误还会附带宏定义和调用来源。

## 8. 支持范围与当前限制

本次基线支持：

- module、普通及带方向的端口列表、模块参数列表。
- 参数、局部参数、变量、net、端口及 branch 声明；属性；参数区间和数组范围。
- 基本 nature/discipline 定义及其属性条目。
- analog 块、analog function、begin/end、if/else、case、for、while、repeat 和模拟事件语句。
- 赋值、贡献运算 <+、调用语句，以及基本跳转语句的语法形态。
- 数字、字符串、名称、一元与二元运算、条件表达式、括号、函数调用、下标、成员、连接与重复连接表达式。

这些能力是语法识别，没有做合法属性名检查、符号绑定、类型推导、单位检查或表达式求值。例如拼错的 discipline 名可能仍有正确的 net 声明语法，需由后续 HIR 报未定义符号。

尚未覆盖完整数字 Verilog、generate、模块实例化、复杂连接规则、全部数组初始化形式及所有 Verilog-AMS 语法。当前没有宣称完整 BSIM-BULK 通过 Parser。表达式和语句递归层数共用 128 的上限，超限会产生 PARSE_DEPTH 诊断。

CST 表达生效配置下的展开代码。未选中的条件分支保存在原始文件快照中，不作为这棵展开 CST 的节点。AST View 与 HIR H1 名称绑定已接入，尚未实现有类型的 HIR、MIR 或自动微分。

## 9. 代码入口和验收

| 文件 | 职责 |
|---|---|
| `include/vagrad/frontend/parser/parse_event.hpp` | 事件种类和字段 |
| `include/vagrad/frontend/parser/parser.hpp` | ParseResult 和 parse 接口 |
| `src/frontend/parser/parser.cpp` | 递归下降、Pratt、事件标记和错误恢复 |
| `src/frontend/parser/parse_result.cpp` | 错误检查和事件导出 |
| `include/vagrad/frontend/syntax/syntax_kind.hpp` | CST 节点种类 |
| `include/vagrad/frontend/syntax/syntax_tree.hpp` | 节点编号、只读树和构树接口 |
| `src/frontend/syntax/syntax_tree.cpp` | TreeBuilder、文本重建和 CST 导出 |
| `tests/parser_tests.cpp` | 结合方向、来源、恢复、无损构树和生命周期测试 |
| `tests/cli_parser_modes.cmake` | 配置传递、错误退出、导出和输入覆盖保护 |

Q1 接入后全项目 37 项 CTest 已通过。Parser 测试覆盖默认和替代配置、运算符结构、错误后继续解析、空文件、注释与 BOM、2,000 个二元运算的长表达式、嵌套限制和错误事件流检查。每个测试 CST 均检查每枚输入 token 按顺序出现一次，根文本与预处理文本完全一致。

这份 CST 现可通过类型化 AST View 读取，避免后续代码直接依赖子节点的固定下标，见 [ast-view.md](ast-view.md)。
