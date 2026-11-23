# Preprocessor 基线使用与实现

## 1. 这一步得到什么

输入为源文件的原始 token，输出为展开后的 token 流及来源上下文。整个结果保存在内存中的 `PreprocessResult`，只有使用 CLI 的 `-o` 才会保存为文件。

输入示例：

```verilog
`include "include/simple_mos_defaults.vams"
`define SQUARE(x) ((x) * (x))
parameter real G0 = `DEFAULT_GAIN from (0:inf);
ids = .5 * G0 * `SQUARE(vov);
```

输出中的有效代码：

```verilog
parameter real G0 = 1m from (0:inf);
ids = .5 * G0 * ((vov) * (vov));
```

`define` 用来更新宏表，完成后不再作为模型代码输出。include 的内容在引用位置被处理。预处理器还不生成 AST、绑定参数或执行电流公式。

## 2. 命令

在 PowerShell 运行：

```powershell
Set-Location 'E:\OpenVAF\VA-Gradient'

# 编译并运行全部测试。
.\scripts\build.ps1

# 保存展开后的代码。
.\build\windows-msvc\vagrad.exe preprocess .\testdata\simple_mos.va -o .\build\simple_mos.expanded.va

# 保存一份合并 token 列表，含主文件、头文件和宏的来源信息。
.\build\windows-msvc\vagrad.exe preprocess .\testdata\simple_mos.va --dump-tokens -o .\build\simple_mos.tokens.txt

# 改变条件编译配置，结果直接显示在终端。
.\build\windows-msvc\vagrad.exe preprocess .\testdata\simple_mos.va -D SOFT_THRESHOLD -D TRACE_MOS
```

`-D NAME` 等价于定义 `NAME=1`；`-D NAME=VALUE` 指定替换文字，支持空值。也支持 `-DNAME=VALUE` 的连续写法。相同名字重复指定时使用最后一个值。

`-I directory` 可多次提供；也支持 `-Idirectory`。先相对于当前文件寻找 include，再按传入顺序搜索这些目录。`-I` 中的相对目录以运行命令时的工作目录为基准。

正确处理返回 0，预处理或写文件失败返回 1，命令格式错误返回 2。出现预处理错误时输出可能是供排错的部分结果，应先检查退出码再交给后续步骤。输出路径不得指向已加载的主文件或头文件。

`lex` 只对当前输入文件做词法分析，会保留 include、宏和条件指令，不读取头文件。include 展开统一由 `preprocess` 处理；需要得到后续 Parser 的输入时直接使用 `preprocess`，无需先执行 lex 命令。

## 3. 代码怎样工作

入口为 `preprocess()`，它创建一次运行所需的宏表与结果对象。

1. 登记根文件；将 `-D` 宏登记到宏表。命令行宏也有只读虚拟文件快照，可定位其定义。
2. `process_file()` 调用 Lexer 得到当前文件的原始 token，按顺序扫描。
3. 普通且生效的 token 暂存到 `chunk`。碰到预处理指令时先展开并提交之前的 chunk，确保后面的定义不能反过来影响前面的代码。
4. `define_macro()` 保存宏名、形参、宏体 token、定义位置及定义时的来源上下文。宏体此时没有求值。
5. `change_condition()` 用栈记录当前分支是否生效。只有生效分支才能更新宏表或打开头文件。普通模型 `if (...)` 与此无关，会保留在输出中。
6. `include()` 展开文件名中的宏，解析路径，建立新的 Include 上下文，再递归处理头文件。返回后继续当前文件；各文件共享同一宏表。
7. `expand()` 读取宏调用、配对括号并分割实参，先展开实参，再替换宏体中的形参。替换结果回到待处理队列前方继续扫描，因此支持嵌套和别名。
8. `append()` 记录输出范围与原始范围；必要时补空白，防止两个相邻替换结果意外粘成另一个 token。整个结果最后只有根文件的一枚 EOF。

例如 `SQUARE(vov)` 中，`x` 两次被替换为 `vov`。生成的两个 `vov` 都回指调用处的同一段原文，但分别具有不同的实参上下文，因此能区分“复制后的哪一次使用”。

宏体的再次扫描示例：

```verilog
`define ID(x) x
`define ALIAS `ID
real a = `ALIAS(2);
```

`ALIAS` 先成为 `ID`，队列里还保留 `(2)`，随后按带参数宏处理，最后成为 `real a = 2;`。

## 4. 来源映射怎么看

每枚输出 token 包含：

| 字段 | 含义 |
|---|---|
| `original.kind` | Lexer 中的 token 种类 |
| `original.span` | 原始文件编号与原始字节范围 |
| `context` | 这次 include、宏展开或实参替换的上下文编号 |
| `begin/end` | token 在展开流中的字节范围 |

宏体 token 没有被改写到原始文件里。输出通过 span 引用原始文件快照，重复使用可以共享同一段文字。

以 ``G0 = `DEFAULT_GAIN`` 为例，输出的 `1m` 来自头文件定义；调用位置则在主文件：

```text
File[0] 主文件 simple_mos.va
File[1] 头文件 simple_mos_defaults.vams
Context[1] File[1] Parent[0] at File[0] 9:5
Context[2] File[1] Macro "DEFAULT_GAIN" Parent[0] at File[0] 28:25
           Defined at File[1] 6:9 DefinitionContext[1]
```

`Parent[0]` 指向调用时的上下文，`DefinitionContext[1]` 指向定义时实际进入头文件的那一次包含。这样即使同一个文件被多次引用，也能保留定义来源。

实参还会出现类似记录：

```text
Argument "SQUARE" Parent[11] ... Formal at File[0] 12:25 OriginContext[0]
```

这里 `Parent[11]` 表示它属于某次 SQUARE 展开，`Formal` 指宏体中被替换的形参位置，`OriginContext[0]` 指实参替换前的来源。嵌套宏的 OriginContext 还可能指向更早的宏或实参上下文。因此来源信息形成有多种边的图，不能只靠展开后的行号反推。

`Expanded[a,b)` 采用左闭右开范围，表示导出的展开文本中的位置；后面的原始行列和 `[begin,end)` 则属于原文件。不能将展开位置直接当作主文件偏移。

## 5. 本次实现范围

已实现对象宏、带参数宏、零参数和空实参、多行宏、嵌套展开、`undef`、`undefineall`，以及 `ifdef / ifndef / elsif / else / endif`。宏定义和调用区分大小写。宏参数按 token 替换，不改动字符串和注释中的同名文字。

`ifdef` 检查名字是否已定义，不求宏值的真假。例如 `-D SOFT_THRESHOLD=0` 仍会选择 SOFT_THRESHOLD 分支。

支持包含保护、重复包含、嵌套相对路径、`-I` 搜索目录、以及展开为一个字符串 token 的 include 文件名。用“文件 + 宏表内容”检查包含环，允许有保护宏的自包含；另有 128 层包含、128 层实参嵌套和每次运行 100 万次 token 处理的限制。

当前仍是项目基线，不是完整 Verilog-A 预处理兼容性实现。尚未支持 token 粘接、宏字符串构造、默认宏实参、预定义的 `__FILE__/__LINE__`，以及 `line`、`timescale` 等其他编译器指令；这类有效分支中的未知引用会报错。条件指令在每个文件内配对，不支持跨文件开启和关闭同一个条件块，也不在宏展开结果中执行新的预处理指令。

常量头文件、discipline 定义等仍需要提供实际 include 文件或 `-I` 路径。本次没有自动引入 OpenVAF 的内置头文件，也没有宣称完整 BSIM-BULK 已通过预处理。

## 6. 实现文件与验证

| 文件 | 职责 |
|---|---|
| `include/vagrad/frontend/preprocessor/preprocessor.hpp` | 对外入口、配置、PreprocessResult、来源结构和导出接口 |
| `include/vagrad/frontend/preprocessor/macro_table.hpp` | 宏和宏体 token 的内存结构 |
| `src/frontend/preprocessor/preprocessor.cpp` | 按顺序处理文件、条件栈、宏展开和来源图 |
| `src/frontend/preprocessor/include_resolver.cpp` | 当前目录和 `-I` 的文件查找 |
| `src/frontend/preprocessor/preprocess_result.cpp` | 结果错误检查、展开代码与来源 token 列表的输出 |
| `tools/vagrad_main.cpp` | 参数解析、输出保护和诊断打印 |
| `tests/preprocessor_tests.cpp` | 预处理验收用例 |
| `tests/cli_modes.cmake` | 验证 CLI 两个阶段的职责、合并输出和输入覆盖保护 |

预处理用例核对宏别名、嵌套实参、分支选择、未生效的 include、嵌套与重复包含、包含保护、错误诊断、来源图和快照寿命。展开文本重新 Lexer 后，其有效 token 序列必须与预处理结果一致。Q1 接入后，全项目共有 37 项 CTest。

当前默认示例展开得到 486 个 token（含空白、注释和 EOF）、17 个来源上下文及 1,713 字节展开文本，没有未展开的 Directive token。实际 CLI 还核对了 `-D` 分支切换、`-I` 参数和输入文件覆盖保护。

展开 token 现在可直接交给 `parse()`，再由 TreeBuilder 生成 CST，见 [parser.md](parser.md)。预处理 token、ParseEvent、CST 与 AST View 导出共用 `print_source_map()`，不会重建另一套来源图。被条件编译移除的分支仍保留在原始文件快照中，但不会成为这份展开语法树的语法节点。
