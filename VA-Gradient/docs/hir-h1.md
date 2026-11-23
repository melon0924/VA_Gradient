# HIR H1：作用域、符号表与名称绑定

完成日期：2026-11-19。当前实现 H1 名称绑定，H2 类型/参数求值与 H3 解释器尚未接入。

## 1. 本步解决的问题

AST View 已经知道某段代码是 NameExpr、AssignmentStmt 或 ParameterDecl。H1 进一步确定：这个名称指向哪一处定义，它属于哪个作用域，这次出现是读取、写入、调用还是类型引用。

```verilog
parameter real G0 = 1m;
real ids;
analog begin
    ids = G0 * 2.0;
end
```

H1 为 G0 和 ids 创建定义，表达式中的名称通过 SymbolId 指向定义：

```text
Symbol[...] Parameter "G0"
Symbol[...] Variable "ids"
Ref[...] Write "ids" -> Symbol[...]
Ref[...] Read  "G0"  -> Symbol[...]
```

重复给 ids 赋值仍绑定同一个 SymbolId。此时还没有把每次赋值转换为 SSA ValueId，也没有计算电流或导数。

## 2. 运行及结果位置

```powershell
Set-Location 'E:\OpenVAF\VA-Gradient'
.\scripts\build.ps1

# 自动执行已有预处理、Parser、构树，再进行 H1。
.\build\windows-msvc\vagrad.exe hir .\testdata\simple_mos.va -o .\build\simple_mos.hir.txt

# 使用替代宏配置。
.\build\windows-msvc\vagrad.exe hir .\testdata\simple_mos.va -D SOFT_THRESHOLD -D TRACE_MOS

# 语法正确但有三处名称问题的样例，预期退出码为 1。
.\build\windows-msvc\vagrad.exe hir .\testdata\semantic\binding_errors.va
```

默认结果保存在 `build/simple_mos.hir.txt`。输出仍为 TXT 调试报告，内部不通过读取该报告传递数据。

默认样例验收：

| 项目 | 结果 |
|---|---:|
| 作用域 | 4：Prelude、CompilationUnit、Module、NamedBlock |
| 用户符号 | 18 |
| 名称引用 | 39 |
| 成功绑定 | 39 |
| H1 错误 | 0 |

Prelude 当前登记 67 个内建项目，主要包括 electrical、V/I、常见数学函数、环境名和诊断/模拟运算名。登记表示 H1 能识别名称与类别，并不表示已经实现该函数的数值执行。

## 3. 处理流程

```text
已有 AST View / CST
    ↓
第一遍：收集声明、创建作用域、合并端口信息
    ↓
标记引用角色：Read / Write / Call / Type / Terminal ...
    ↓
第二遍：从当前作用域向上查找定义，检查引用种类和作用域限制
    ↓
BindingResult：作用域 + 符号 + 引用 + 诊断
```

两个遍历都基于已有节点和 token。BindingResult 共享同一棵只读 CST，保留原始文件和宏上下文，不重新运行另一套词法分析或 include 机制。

### 作用域

模块、命名块、函数、nature 和 discipline 有各自的作用域记录。普通未命名 begin/end 只组织语句，不建立额外词法作用域。命名块和函数的局部定义优先于外层定义。

```verilog
parameter real P = 1;
analog begin : outer
    real x;
    begin : inner
        real P;
        x = P;   // P 指向 inner 中的变量。
    end
    x = P;       // P 指向模块参数。
end
```

同作用域的重复变量、参数、函数或命名块会得到诊断。模块之间的普通变量不会互相泄漏。

### 端口和函数参数合并

```verilog
module simple_mos(d, g, s);
inout d, g, s;
electrical d, g, s;
```

d 的三处声明完善同一个 Net 符号，分别记录 PortHeader、Direction 和 DataType 来源。g/s 同理。

函数中的 `input x; real x;` 也完善同一个形参，而不是创建两个 x。ANSI 端口组 `input real a,b` 中，b 继承同组的方向和类型；新的显式方向开始新组，未写出的类型不沿用上一组。

方向或类型相互冲突，以及重复声明同一声明成分，均会报错。普通变量重复定义不会被当作端口补充声明合并。

### 函数及特殊名字

函数拥有函数符号和内部同名 FunctionResult 符号：`f = ...` 绑定返回变量，`f(...)` 绑定函数。局部形参和局部参数可以遮蔽模块参数，模块参数仍可在函数中读取。

H1 会拒绝函数捕获模块变量/net/branch、函数内使用 access/filter 调用，以及函数中的命名块。它还不检查完整函数签名、参数方向效果和递归调用图，这些属于后续语义工作。

全局 nature/discipline 使用类型名称表，access 属性声明可调用的访问名。potential/flow、nature 继承与 ddt_nature/idt_nature 等位置按 nature 名称引用处理。显式定义优先于备用 Prelude；access 名的模块可见性检查其定义先后。

转义标识符会去掉标识用的反斜杠后再比较，因此 `\abc` 与 `abc` 绑定同一个定义。规则依据 [LRM 第 2.8.1、3.13、4.7 和 6.8 节](https://www.accellera.org/images/downloads/standards/v-ams/VAMS-LRM-2-4.pdf)。

## 4. 核心数据结构

公共接口位于 `include/vagrad/frontend/hir/hir.hpp`，命名空间为 `vagrad::hir`。

| 结构 | 主要字段 |
|---|---|
| ScopeId / SymbolId / ReferenceId | 各自存储内的强类型编号 |
| Origin | CST NodeId 与展开 token 索引 |
| Scope | 种类、父子作用域、所属节点/符号、值名称表、类型名称表 |
| Symbol | 规范化名字、种类、作用域、声明来源、端口方向及声明类型文字 |
| Reference | 名字、角色、作用域、来源、目标 SymbolId 与状态 |
| BindingResult | 共享 CST、作用域/符号/引用数组、节点作用域映射及诊断 |

值名称与类型名称分开查询。模块、nature、discipline 在编译单元类型表中登记；参数、变量、net、branch、函数和命名块在相应值名称表中登记。

Symbol.type_name 当前只是声明文字，不是 H2 的完整类型。数值默认值、参数依赖、转换、单位和表达式类型仍保存在前序语法中等待后续处理。

ReferenceRole 区分读取、写入、读写更新、调用、discipline/nature 类型、探针/branch 端点、domain、disable 目标和结束标签。对于 `a[i] += 1`，a 是 ReadWrite，i 是 Read。

ReferenceStatus 可能为 Resolved、Undefined、WrongKind 或 Forbidden。出现错误种类或非法函数捕获时，target 仍可指向找到的定义，便于展示原因；不能仅凭 target 有值就认定该引用合法。

## 5. C++ 使用

```cpp
// sources、file 沿用前面的 SourceManager 与文件编号。
auto input = std::make_shared<const vagrad::PreprocessResult>(
    vagrad::preprocess(sources, file)
);
if (input->has_errors()) return;

auto parsed = vagrad::parse(input);
if (parsed.has_errors()) return;

auto tree = vagrad::build_syntax_tree(parsed);
auto bindings = vagrad::hir::bind_names(tree);

for (const auto& reference : bindings.references) {
    if (reference.status == vagrad::hir::ReferenceStatus::Resolved) {
        const auto& definition = bindings.symbol(*reference.target);
        // definition.name、definition.scope、definition.declarations ...
    }
}
```

调用 bind_names 前必须检查 ParseResult.has_errors()。库还会拒绝含 ErrorNode 的树，但漏分号等语法错误未必创建 ErrorNode；正式 hir 命令会完整检查上游诊断。

BindingResult.lookup 提供原始词法向上查询，适合查表和调试；它不代替实际引用绑定中的调用上下文、access 定义先后和函数捕获检查。后续降低应直接使用 Reference 的目标及状态。

declared_at(token_index) 返回该 token 关联的声明符号列表。一个函数名字同时关联显式函数定义与隐式返回变量，因此返回列表。reference_at(token_index) 用于查询某次引用出现。

## 6. 阅读 H1 报告

文件开头沿用 File/Context 来源表，随后输出 Scope、Symbol、Declaration、Ref 和 Summary。

默认样例中的简化示意：

```text
Scope[2] Module "simple_mos" Parent[1]
Scope[3] NamedBlock "evaluate_current" Parent[2]

Symbol[...] Net "d" ... port=true direction=Inout declared_type="electrical"
  Declaration PortHeader ...
  Declaration Direction "Inout" ...
  Declaration DataType "electrical" ...

Symbol[...] Variable "ids" Scope[2]
Ref[...] Write "ids" Scope[3] -> Symbol[...] Variable Status=Resolved ...
Ref[...] Read "ids" Scope[3] -> Symbol[...] Variable Status=Resolved ...
```

具体编号随输入和 Prelude 项目变化，使用时应查表。原始 SourceSpan 相同的宏实参复制仍有不同的展开 token 索引与 CST 节点，绑定时不会误合并为同一次引用。

默认模型最终 Summary：

```text
scopes=4 user_symbols=18 builtin_symbols=67 references=39 resolved=39 errors=0
```

## 7. 诊断与验证

错误样例 `testdata/semantic/binding_errors.va` 有三处 H1 问题：重复 G0、未定义 missing_voltage、未定义 unknown_fn。重复定义会关联先前位置，宏内未知名字会附带展开/定义上下文。

预处理或语法失败时，hir 不生成绑定报告，也不会替换先前输出；H1 自身有错时仍可导出部分绑定结果，退出码为 1，报告保留未解析项和诊断。成功为 0，命令格式错误为 2。

全项目 18 项 CTest 已通过。新增测试覆盖：

- 默认与 SOFT_THRESHOLD/TRACE_MOS 配置、端口三处合并与 ANSI 继承。
- 命名块遮蔽、普通块作用域、结束标签、disable、函数形参和返回变量。
- nature/discipline、内建与自定义访问名、转义名称、读写角色。
- 重复定义、方向/类型冲突、未定义名、错误种类及不支持的层次引用。
- 宏出现位置、来源链、CST 共享、对象生命周期和部分结果结构约束。
- CLI 配置传递、错误退出、旧输出保留、输入覆盖保护。

| 文件 | 职责 |
|---|---|
| `include/vagrad/frontend/hir/hir.hpp` | H1 数据结构、查询及入口 |
| `src/frontend/hir/binding.cpp` | 声明收集、作用域、角色与引用绑定 |
| `src/frontend/hir/hir.cpp` | 查询、状态与种类名称 |
| `src/frontend/hir/hir_dump.cpp` | 可读报告 |
| `tests/hir_tests.cpp` | 库层验收 |
| `tests/cli_hir_modes.cmake` | CLI 集成验收 |

## 8. 当前边界与下一步

H1 完成名称解析与必要的声明/引用种类检查。层次名、成员路径等目前给出明确的未支持诊断。未命名块中的声明、非法局部 net/branch 等位置也会被诊断。

H1 不做完整表达式类型检查、参数求值和范围验证，不执行变量、函数或模拟运算，也不检查完整函数递归/副作用语义。因此“绑定成功”不等于“模型已经可以正确数值执行”。

后续 H2/H3 静态标量基线已于 2026-11-19 接入，复用这些 SymbolId 和 Reference。H1 自身的报告和接口仍保持名称绑定职责。类型、参数与参考执行见 [hir-h2-h3.md](E:/OpenVAF/VA-Gradient/docs/hir-h2-h3.md)；Q1 接入后全项目现有 37 项测试通过。
