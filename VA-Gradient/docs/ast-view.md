# AST View：在 CST 上读取类型化语法

## 1. 本阶段的作用

当前前端链路：

```text
源码 → Lexer → Preprocessor → Parser → ParseEvent → TreeBuilder → CST
                                                                ↓
                                                            AST View
                                                                ↓
                                                     HIR H1 / 名称绑定
```

AST View 是 CST 的类型化访问层。它共享已有树和源文件快照，使用同一份 NodeId；不会重新分析源码、展开 include 或分配第二棵语法树。

过去读取一个赋值节点，需要检查原始 children，区分空白、表达式、运算符和分号。现在可以写：

```cpp
assignment.lhs();  // 左侧表达式
assignment.op();   // =、<+ 或其他赋值运算符
assignment.rhs();  // 右侧表达式
```

字段查找的细节集中在 AST View 内部。后续 HIR 可以通过这些接口读取语法，避免在各处依赖固定的 children 下标。

## 2. 已生成的结果

```powershell
Set-Location 'E:\OpenVAF\VA-Gradient'

# 构建并运行全部 15 项测试。
.\scripts\build.ps1

# 自动预处理、解析、构树，再导出类型化视图。
.\build\windows-msvc\vagrad.exe ast .\testdata\simple_mos.va -o .\build\simple_mos.ast.txt

# 条件编译配置和 include 搜索目录沿用原有参数。
.\build\windows-msvc\vagrad.exe ast .\testdata\simple_mos.va -D SOFT_THRESHOLD -D TRACE_MOS
```

不指定 `-o` 时显示在终端。输出继续使用 TXT，位于 `build/simple_mos.ast.txt`，不要求预先保存 token、事件或 CST 文件。

默认模型的实际结果：

| 内容 | 数量 |
|---|---:|
| ModuleDecl | 1 |
| ParameterDecl | 7 |
| AssignmentStmt | 7 |
| IfStmt | 2 |
| ContributionStmt | 1 |
| 导出的语法节点记录 | 159 |

节点记录数量与当前 CST 相同，因为每条记录都对应已有节点；导出省略了独立的空白、注释和标点 token 行，并用字段展示名称、运算符和角色。CST 内的全部 486 枚 token 仍然完整保留。

既有 ParseEvent 与 CST 导出内容已逐字节核对，未发生变化。

## 3. 阅读输出

省略部分位置字段后，参数示例如下：

```text
ParameterDecl keyword="parameter" type="real" Node[18]
  AttributeList ...
  Declarator name="G0" Node[24]
    initializer: LiteralExpr value="1m" Node[25]
    ParameterRange keyword="from" lower_inclusive=false upper_inclusive=false
      lower: LiteralExpr value="0"
      upper: LiteralExpr value="inf"
```

这里 ParameterDecl 表达整条参数声明，Declarator 表达其中的一个参数名字及其默认值。一条声明可以定义多个名字，因此 name 和 initializer 放在 Declarator 上。

`initializer`、`lower`、`upper` 是 AST View 读取到的字段角色。`1m` 保持原始数字写法，此时尚未计算为浮点数。

表达式示意：

```text
AssignmentStmt op="="
  lhs: NameExpr name="ids"
  rhs: BinaryExpr op="*"
    lhs: NameExpr name="LEAK"
    rhs: NameExpr name="vds"
```

`Node[n]` 是底层 CST 的节点编号，`Expanded[a,b)` 是展开文本范围。`Origin(File[n] 行:列 Context[n])` 标记节点内首枚有效 token 的原始位置；宏展开节点可能来自头文件或宏体，原始位置不一定属于主文件当前行。

开头的 File/Context 表沿用预处理器的来源图。文件和上下文编号取决于本次输入及宏配置，应通过表查询，不能假设头文件永远为 File[1]。

## 4. C++ 调用示例

下面展示读取模块参数，不依赖导出的文本文件：

```cpp
#include "vagrad/frontend/ast/ast.hpp"
#include "vagrad/frontend/parser/parser.hpp"
#include <iostream>
#include <memory>

void inspect_model() {
    using namespace vagrad;

    SourceManager sources;
    const auto file = sources.load(
        "E:/OpenVAF/VA-Gradient/testdata/simple_mos.va"
    );
    auto input = std::make_shared<const PreprocessResult>(
        preprocess(sources, file)
    );
    if (input->has_errors()) return;

    auto parsed = parse(input);
    if (parsed.has_errors()) return;

    auto tree = build_syntax_tree(parsed);
    auto root = ast::Root::from_tree(tree);

    for (const auto& module : root.modules()) {
        if (const auto name = module.name()) {
            std::cout << "module: " << name->text() << '\n';
        }

        for (const auto& declaration : module.parameters()) {
            for (const auto& parameter : declaration.declarators()) {
                const auto name = parameter.name();
                const auto initial = parameter.initializer();
                if (name && initial) {
                    std::cout << name->text() << " = "
                              << initial->text() << '\n';
                }
            }
        }
    }
}
```

`initial->text()` 返回该节点的完整展开文字，可能包含前导空白。读取名称或字面量时，可以使用对应 token 的 `text()`，它只返回该 token 的原文。

## 5. Node、Token 和类型转换

`ast::Node` 保存 SyntaxTree 句柄和 NodeId；SyntaxTree 自身共享只读存储。复制一个 View 只复制句柄，不复制所有节点。

| 接口 | 含义 |
|---|---|
| `node.kind()` | CST 节点种类 |
| `node.children()` | 直接子节点，非递归 |
| `node.tokens()` | 直接的非 trivia token，非递归 |
| `node.text()` | 整个节点的展开文字，含保留的 trivia |
| `node.range()` | 展开流中的字节范围 |
| `node.attributes()` | 直接附着的属性，不搜索后代 |
| `node.first_token()` | 子树首枚有效 token，跳过 EOF |
| `node.contains_error_node()` | 子树中是否存在 ErrorNode 占位 |

`ast::Token` 保存树句柄与展开 token 索引。它的 `source_span()`、`context_index()` 和 `context()` 直接访问原来的来源信息。其 `text()` 是只读 string_view；长期保存字符串视图时要保留相应 Token/Node/Tree，或者复制成 string。

类型转换采用：

```cpp
if (const auto binary = ast::cast<ast::BinaryExpr>(some_node)) {
    auto left = binary->lhs();
    auto right = binary->rhs();
}
```

`cast<T>()` 检查节点种类，失败返回 `std::nullopt`。直接用错误种类构造类型化节点，例如 `ast::BinaryExpr(module_node)`，会抛出 invalid_argument。

`TypedNode<Expected, Base>` 是实现内部的公共模板，用来集中完成这种检查。各具体 View 在它上面增加字段接口；读调用代码时可以先关注 Module、Declarator、Expr 和 Statement。

## 6. 已提供的主要字段

| 类型 | 常用接口 |
|---|---|
| Root | modules、natures、disciplines |
| Module | name、ports、parameters、declarations、branches、functions、analog_blocks |
| Declaration | keyword、data_type、declarators |
| Declarator | name、initializer、array_ranges、parameter_ranges |
| Port | name、direction、data_type、default_value |
| BranchDecl | names、terminals |
| AnalogFunction | name、return_type、ports、declarations、statements |
| AnalogBlock | is_initial、body |
| BlockStmt | label、items、declarations、statements |
| Assignment / ContributionStmt | op、lhs、rhs |
| IfStmt | condition、then_branch、else_branch |
| ForStmt | initializer、condition、step、body |
| WhileStmt / RepeatStmt | condition 或 count、body |
| CaseStmt / CaseItem | selector、items、labels、is_default、body |
| EventStmt | events、body |
| NameExpr / LiteralExpr | name 或 literal |
| BinaryExpr / UnaryExpr | op、lhs/rhs 或 operand |
| ConditionalExpr | condition、true_value、false_value |
| CallExpr | callee、arguments |
| IndexExpr / MemberExpr | base、index/range_end 或 member |
| ParameterRange | keyword、lower、upper、端点包含性、single_value |

Module.parameters 包括模块头 `#(...)` 及模块体参数，保持出现顺序，不包含函数内部的局部参数。Module.declarations 只返回模块体中的直接声明。类似地，BlockStmt.statements 不会把嵌套分支里的语句混进当前块的直接语句列表。

`Expr.without_parens()` 允许调用方穿过括号节点读取内部表达式，原树中的括号和 token 仍然保留。

AST 字段匹配依据当前节点的直接子节点和对应分隔符，不会递归搜索一个碰巧类型相同的后代来填充字段。

## 7. 不完整语法与来源身份

字段可能有三种情况：

1. 正常存在：返回类型化节点或 token。
2. 可选字段不存在，或缺少足够的语法标记：返回 nullopt。
3. Parser 已放入 ErrorNode 占位：返回 kind 为 ErrorNode 的 Expr/Statement。

例如：

```verilog
parameter real P from [0:1];  // 没有 '=' 和默认值
ids = a + ;                  // Parser 为缺失的右操作数创建 ErrorNode
```

第一行的 initializer 返回 nullopt，不会误取参数范围中的 0。第二行 BinaryExpr.rhs 返回 ErrorNode 表达式，便于保留错误位置。

`contains_error_node()` 不是完整的语法正确性检查。例如漏分号可以只产生诊断而不创建 ErrorNode，整体是否成功仍应检查 ParseResult.has_errors()。错误样例 `testdata/parser_errors.va` 可以通过 ast 命令导出部分 View，并返回退出码 1。

宏替换还会产生一个重要情况：

```verilog
`define SQUARE(x) ((x) * (x))
ids = `SQUARE(vds);
```

两个展开的 vds 都可以回指同一个原始实参位置，但它们是不同的 token 出现位置和不同的语法节点。View 的相等性依据底层树节点身份；Token 依据展开 token 身份。不能仅用原始 SourceSpan 判断它们是不是同一个节点。

节点编号只在所属树内有效。只读树、View 和 Token 的生命周期已有测试，原来的局部 SourceManager、ParseResult 或 tree 变量退出作用域后，保留的 View 仍可读取数据。

## 8. 验证、实现位置与后续

Q1 接入后全项目 37 项 CTest 通过。AST View 测试覆盖模型默认及替代配置、声明作用域、多个 declarator、表达式字段、循环和分支槽位、宏来源、复制实参身份、缺失字段、View 生命周期、只读存储共享和长表达式遍历。CLI 验证输出、配置传递、错误返回以及输入覆盖保护。

| 文件 | 职责 |
|---|---|
| `include/vagrad/frontend/ast/ast.hpp` | Node/Token、类型化 View 和公共字段接口 |
| `src/frontend/ast/ast.cpp` | 基于已有 CST 的字段读取 |
| `src/frontend/ast/ast_dump.cpp` | 使用类型化接口导出可读结果 |
| `tests/ast_tests.cpp` | View 的行为、来源、错误容忍和存储共享测试 |
| `tests/cli_ast_modes.cmake` | CLI 集成验收 |

当前 View 的语法范围随 Parser，列表容器也可以通过通用 Node 读取。AST View 不扩大 Parser 的语言覆盖范围。

这里的类型化指“把节点解释成模块、声明、表达式或语句”。H1 已在该 View 上完成名称绑定，见 [hir-h1.md](hir-h1.md)；数据类型推导、单位检查、公式求值或自动微分仍待后续实施。
