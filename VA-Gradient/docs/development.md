# 开发进度与接口约定

## 第一阶段

2026 年 11 月 18 日完成第一阶段。`vagrad_frontend` 是公共库，CLI 和测试均链接同一实现。正式源码放在 `include`、`src` 和 `tools`，测试入口放在 `tests`，构建文件由 CMake 生成到 `build/windows-msvc`。

SourceManager 规范化物理路径后去重，加载时建立文件内容快照。FileId 只在所属 SourceManager 内有效。失败的加载不会登记文件。快照同时拥有文本和行起点索引，可由后续 SyntaxTree 共享持有。

SourceSpan 对应单一原始文件，范围采用字节偏移。后续引入预处理时，为展开流增加独立的范围和来源映射。这样，宏展开后的节点可用展开流位置遍历，同时通过映射查询定义位置和调用位置。

## 第一阶段验收

构建脚本已在当前 Windows 机器上运行。CTest 注册了三个测试，全部通过。

- `common` 检查换行、文件位置、快照寿命、失败状态和诊断。
- `cli_source` 检查示例文件能经正式 CLI 加载并显示行数。
- `cli_locate` 检查正式 CLI 能将偏移零映射到第一行第一列。

## 第二阶段的 Lexer

2026 年 11 月 18 日完成 Lexer。Token 保存种类和 SourceSpan，LexResult 共享持有源文件快照，同时返回诊断。空白、注释、换行、UTF-8 BOM 和续行都有对应 token。每个非 EOF token 都覆盖一段非空原文，前后范围连续，EOF 位于文件末尾。

词法错误通过 Error token 和 Diagnostic 表达，并继续扫描。后续 Parser 可从保留完整原文的 token 流中选择有效记号，同时让 TreeBuilder 使用完整顺序构树。

新增命令为 `vagrad lex <file.va> [-o <tokens.txt>]`。5 项 CTest 全部通过，覆盖分类、长运算符优先匹配、数值形式、字符串转义、位置、无损原文和错误后继续扫描。

实际模型验证如下，token 数含 trivia 和末尾 EOF。

| 输入 | 字节数 | token 数 | 验收 |
|---|---:|---:|---|
| 原始 bsimbulk.va | 337,279 | 80,773 | 词法分析成功，按序拼接与文件字节一致 |
| bsimbulk.expanded.va | 568,560 | 146,044 | 词法分析成功，按序拼接与文件字节一致 |

原始文件中的宏和条件编译指令保留为 Directive token。这个阶段的结果是原始 token 流。

## 增强 simple_mos 样例

2026 年 11 月 18 日，将初始样例扩展到 60 个物理行和 2,031 字节。包含中文注释、注释中的伪宏、7 个宏定义、多行宏与嵌套调用、条件编译、带转义的字符串、科学计数法和工程单位后缀。

运行实际 CLI 后得到 599 个 token，其中 20 个 Directive、5 个 LineComment、1 个 BlockComment、1 个 LineContinuation，词法错误数为零。输出 token 的文字和范围已经逐字节验证能够重建输入。

CTest 的 lexer 用例增加了对增强样例的分类检查，确保注释中的伪宏、条件编译和多行宏得到预期处理。全部 5 项 CTest 通过。CLI 的源码加载用例改为检查稳定的 FileId，样例行数由 SourceManager 根据文件内容计算。

## 为预处理准备包含文件

2026 年 11 月 18 日，将默认参数宏移到 `testdata/include/simple_mos_defaults.vams`。主文件用相对路径引用它，保留本地的带参数宏、条件编译和多行宏。

当前主文件为 58 个物理行、2,018 字节、579 个 token，头文件为 8 个物理行、207 字节、29 个 token。两个文件分别加载，FileId 各自独立。更新后的词法测试核对 include 指令与文件名、头文件中的包含保护及默认参数定义，全部 5 项 CTest 通过。两个输出均能逐字节还原各自的输入。

预处理器的首个验收场景是从主文件目录找到头文件，登记宏，在主文件中将 `DEFAULT_GAIN` 展开成 `1m`、`DEFAULT_VTH` 展开成 `0.45`，并保留宏定义和调用的来源映射。

## 后续顺序

先增加宏预处理，按源码顺序处理宏表和条件编译，并保存每层展开的定义位置、调用位置和实参来源。include 的生效范围随条件编译状态一起处理。该部分已在下述“完整预处理基线”中完成。每次交付由同一个 CLI 暴露相应输出，并加入对应模块的 CTest 验证。

CST、Parser 和 AST View 用最小模型逐步贯通。再对固定版本的 BSIM-BULK 做集成验证，将其数据和大型生成树放在独立的输入与构建目录中。

## include 合并试作（已移除）

曾在 lex 命令中试作独立的 include 合并；完整预处理器接入后，该实现、接口和独立测试目标已删除。嵌套路径、重复包含、包含顺序及错误诊断用例已迁移到统一的预处理测试中。

当前 lex 库接口和 CLI 都只扫描单文件。preprocess 是 include、宏与条件编译的唯一处理入口，合并 token 仍统一输出到 `build/simple_mos.tokens.txt`。

## 完整预处理基线

2026 年 11 月 18 日，增加 `preprocess` 库入口和 CLI 命令。按生效分支与源码顺序共同处理宏、条件编译和 include，不加载未生效分支中的文件。

宏表保存对象宏及函数式宏。替换结果重新进入扫描队列，支持嵌套、别名、配对实参及多行定义。通过每个待处理 token 的禁用宏链检测递归；通过当前文件与宏表内容的组合检测包含环。错误保留可定位的诊断。

PreprocessResult 直接持有文件快照、ExpansionContext、ExpandedToken 和诊断，没有旧结果类型的别名。来源图包括 Root、Include、Macro、Argument、CommandLine、Generated 上下文。调用链用 parent 表达，定义时的上下文用 definition_origin 表达，实参替换前的来源用 argument_origin 表达。所有引用指向先前创建的上下文；输出 token 始终引用已有快照。

默认示例的 DEFAULT_GAIN 与 DEFAULT_VTH 分别展开为 1m 与 0.45，POSITIVE 选择 max 分支，TRACE_MOS 的 $strobe 被移除。用 -D SOFT_THRESHOLD -D TRACE_MOS 则选择 sqrt 分支并保留 $strobe。普通 if 语句不被求值。

8 项 CTest 全部通过。测试检查包含保护、未生效的缺失 include、嵌套与重复包含、命令行宏、宏递归和参数错误，以及展开文本重新词法分析后与内存 token 序列一致。cli_modes 验证 lex 不读取 include，preprocess 统一处理 include 与宏，并保护实际输入免于输出覆盖。实际示例输出统一保存在 build/simple_mos.tokens.txt，展开代码另存为 build/simple_mos.expanded.va，不生成独立头文件 token 文件。

当前支持范围和限制记录在 `docs/preprocessor.md`。下一步搭建 CST 基础与事件构树，再接入 Parser。

## Parser 与 CST 基线

2026 年 11 月 18 日，接入 Parser、ParseEvent 和 TreeBuilder。parse 命令复用同一 Preprocessor，库接口直接共享 PreprocessResult，没有增加 Lexer 或 include 的替代实现。

Parser 通过有效 token 索引读取输入。递归下降处理声明和语句，Pratt 处理表达式；start、complete、precede 产生事件及 forward_parent 边。语法错误由 Error 事件关联 Diagnostic，并通过分号、块边界或下一条明确语句恢复。未知或暂不支持的语法会得到诊断。

TreeBuilder 按事件建树，在 Token 事件前补回 trivia；错误恢复消耗的 token 仍进入 ErrorNode。构树校验有效 token 不遗漏、事件平衡以及来源索引完整。只读 arena 存储通过 shared_ptr 共享，NodeId 用于查询节点，树共享持有原始文件快照和预处理来源图。

默认示例得到 574 个事件和 159 个 CST 节点，包含全部 486 枚展开 token，语法错误为零。默认与 SOFT_THRESHOLD + TRACE_MOS 配置均通过。前序展开代码与 token 输出的字节未变化。

全项目 12 项 CTest 通过，覆盖表达式结构、宏来源、错误恢复、长表达式、递归限制、空输入/trivia、事件完整性、文本重建及存储寿命。CLI 测试覆盖导出、错误退出、配置传递和输入覆盖保护。

实际输出为 build/simple_mos.events.txt 和 build/simple_mos.cst.txt；错误样例位于 testdata/parser_errors.va。实现说明见 `docs/parser.md`。当前是语法与只读树基线，尚未覆盖完整 Verilog-AMS，也未实现增量 red/green 树。下一阶段是类型化 AST View。

## AST View 基线

2026 年 11 月 18 日，接入 namespace vagrad::ast 的类型化视图及 ast 命令。Node 和 Token 句柄共享现有 SyntaxTree 存储，通过 SyntaxKind 检查后提供模块、声明、表达式、语句、参数范围和属性字段。

AST View 读取当前 CST 的直接子节点和分隔符，不重新解析源码或分配第二棵树。模块参数访问包括模块头与模块体，排除函数内的局部声明；表达式和语句接口按角色返回操作数、条件、分支、循环头和主体。

缺失字段采用 optional，Parser 的 ErrorNode 可作为表达式或语句占位保留。View 的错误节点查询不替代 ParseResult 的诊断。节点相等性依据底层树节点身份，宏实参重复展开时即使原始 SourceSpan 相同，也仍有不同的 View 和展开 token 身份。

默认示例导出到 build/simple_mos.ast.txt，包含原 CST 的 159 条节点记录、1 个模块、7 条参数声明、7 条赋值、2 个 if 和 1 条贡献语句。既有 ParseEvent 和 CST 输出逐字节不变。

全项目 15 项 CTest 通过。新增测试检查字段角色、作用域、多个 declarator、宏来源与实参复制身份、缺失字段、只读存储共享、View/Token 寿命及 CLI 保护。实现与 API 使用说明见 `docs/ast-view.md`。

第 5 阶段基线完成，后续按计划扩大真实模型语法覆盖并对接 HIR；当前仍未执行符号绑定、类型推导、模型求值或自动微分。

## HIR H1 名称绑定

2026 年 11 月 19 日完成 H1。新增 ScopeId、SymbolId、ReferenceId、Origin、Scope、Symbol、Reference 和 BindingResult。两遍访问现有 AST View：先建立定义与词法作用域，再按读写、调用、类型和端点等角色绑定引用。

端口头、方向和 discipline 声明合并到同一符号；函数 input/real 声明同样合并，ANSI 端口组支持方向和类型继承。命名块创建作用域，普通块沿用当前作用域。函数拥有同名隐式返回变量，函数局部参数可以遮蔽模块参数。

明确的 Prelude 登记标准环境名称；自定义 nature/discipline/access 名另行收集。重复、冲突、未定义、错误种类、非法函数捕获及未支持层次引用带有定位诊断，宏内错误保留调用和定义链。

hir 命令复用既有预处理、Parser 和构树链。上游语法失败阻止进入 H1；H1 失败仍可导出部分绑定报告。默认模型结果为 4 个作用域、18 个用户符号、39 处引用，全部成功解析，H1 错误为零。

18 项 CTest 通过，新增库与 CLI 验收覆盖默认和替代配置、作用域、声明合并、名称规范化、函数、来源、部分结果结构及输入保护。输出保存于 build/simple_mos.hir.txt；错误样例为 testdata/semantic/binding_errors.va。说明见 `docs/hir-h1.md`。

H1 不包含完整表达式类型、参数求值、模型解释器或 MIR/SSA，下一批为 H2。先前第 1～5 阶段的输出格式与语义保持不变。

## HIR H2 / H3：类型化语义与参考执行

2026 年 11 月 19 日，在 H1 之上接入 typed.hpp、eval.hpp 和对应 typed.cpp、lower.cpp、eval.cpp。lower_hir 消费已有 AST View 及 H1 绑定，建立 Real/Integer/String/Net/Branch/Void/Error 类型、显式 Convert、参数表、内建签名、ReadPotential 和 ContributeFlow。

resolve_parameters 分开处理默认与有效环境，先应用覆盖，再执行剩余默认依赖并验证 from/exclude，检测循环、未知覆盖及类型/范围错误。参数使用保持 Ref，不按默认值提前常量化。

H3 直接解释结构化 HIR，支持本例的 Block/If/Assign/静态贡献与诊断参数收集。调用方提供节点电位和正 kelvin 温度；变量读前须在本次路径赋值。除零、域错误、整数越界、输入缺失和执行超限均带来源诊断。

全部 23 项 CTest 通过，包含原有 18 项回归、新增 H2/H3 库验收及 3 项 CLI 验收。六组独立手算参考覆盖截止、线性、饱和、边界、ENABLE=0 及温度变化；另验收参数覆盖/依赖、宏配置、贡献方向、短路及输出保护。

实际输出保存于 build/simple_mos.typed.hir.txt 和 build/simple_mos.eval.txt；额外参数依赖样例在 testdata/semantic/parameter_dependencies.va。详细说明见 docs/hir-h2-h3.md。

这是单模块、单 analog 块的静态标量执行基线。完整位宽、用户函数、循环/事件、状态运算及完整电路求解仍未实现；超出范围给出能力诊断。下一阶段为 M1/C1，再与 H3 逐点对齐。

## MIR M1 / CFG C1：指令、基本块、校验与执行

2026 年 11 月 19 日，首次构建 MIR-Local 的数据结构及 HIR 降低，24 项 CTest 通过后，因 5 小时额度剩余约 1%，按用户要求保存 docs/mir-checkpoint.md 并停止。用户恢复后，额度窗口已更新，继续从断点构建。

本次补齐 CFG 构建、类型/编号/定义支配使用校验、文本导出、基本块执行器和 CLI。源码变量保留 Local 槽；Value 是指令结果，尚未进行 SSA 局部槽提升。if、&&、||、?: 保留实际控制路径。

为避免两套数值原语分叉，将标量转换、运算和域检查抽到 runtime/scalar.cpp。H3 仍递归执行 HIR 树，MIR 执行器独立沿基本块终结指令跳转，控制流算法相互独立。

默认 simple_mos 生成 10 个基本块、6 个局部槽、47 个 Value。六组手算点、24 个额外偏置点、参数覆盖、替代宏配置及错误状态与 H3 对齐。畸形 MIR 测试覆盖缺失终结、非法编号/类型、重复定义、非支配使用及回边执行上限。

全项目 29 项 CTest 通过。新增 mir/cfg 命令和 eval --ir hir|mir-local，继续复用既有前端链及输入覆盖保护。实际导出在 build/simple_mos.mir.txt、build/simple_mos.cfg.txt、build/simple_mos.mir.eval.txt。说明见 docs/mir.md。

没有扩展 VA 循环、用户函数或状态运算，也没有构造 SSA/phi/切片/AD。下一阶段为 C2/S1。

## SSA C2 / S1：支配分析、phi 与局部槽提升

2026 年 11 月 19 日完成 SSA 基线。新增 analysis/dominance.cpp，提供支配集合、直接支配者、支配树和支配边界，原 MIR 校验器复用该接口。

ssa.cpp 对 Local 计算活跃性，沿迭代支配边界插入 phi，再沿支配树重命名并删除 Load/Store。冗余 phi 按实际 Value 身份简化，参数加载和来源仍保留。SSA Return 显式记录 module 变量出口值。

Undef 表示当前静态求值尚未赋值，不替代为零。require_defined 保留原 Load 的读取检查，包含结果被丢弃的表达式语句。SSA 执行器不分配可变 Local，phi 按实际前驱同时提交，Return 省略未赋值观察项。

默认 simple_mos 的 SSA 为 10 个基本块、42 个 Value、3 个 phi。六组手算、偏置网格、参数覆盖、宏配置及错误状态通过 HIR/MIR-Local/SSA 三方对照；人工循环验证交换、回边、入口前置块和执行上限。33 项 CTest 全部通过。

CLI 新增 mir --form ssa、eval --ir mir-ssa、cfg --dominators。说明在 docs/ssa.md，断点在 docs/ssa-checkpoint.md，小型阅读样例为 testdata/semantic/ssa_branch.va。没有扩大 VA 语法/执行范围，也未开始切片或 AD；下一阶段为 Q1。


## Q1：目标依赖切片

2026 年 11 月 19 日，新增 slice.hpp / slice.cpp，在完整已校验 SSA 上支持 variable@exit 与 flow(branch)@exit。通过后支配关系取得控制依赖，保留 phi 前驱，反向标记数据依赖并重编号。CFG 保留，不输出全局直线求值序。

默认 preserve 保留原输入读取、可能报错的计算与副作用；显式 target-only 提取目标，报告所有效果保留/排除情况。报告分别列出 target_inputs 和 execution_inputs。完整 ParamTable/ParamEnv 仍保留原验证。

默认 simple_mos 的 target-only ids 切片为 32/57 条指令，不需要 TNOM 和温度；flow(ds) 切片为 37/57 条，保留 TNOM 和温度。48 组偏置/温度组合、参数覆盖、控制参数、宏配置、贡献方向及循环切片与原 SSA 目标值对齐。37 项 CTest 通过。

CLI 支持 slice --target ... --effects ...，以及 eval --ir mir-slice。说明为 docs/slice.md，断点为 docs/slice-checkpoint.md。下一步为前向 AD；本次没有计算梯度。

## 前向 AD 第一个检查点（历史，后续已恢复）

2026 年 11 月 19 日新增 ad/forward、ad/plan、ad/state 组件及 ad_primitives 测试。已验证基础链式传播、Real 参数种子、非光滑边界诊断和 phi 导数并行交换。最终 38 项 CTest 通过。

周额度剩余约 1% 时按用户要求停止，断点为 docs/ad-checkpoint.md。尚未接通模型级 SSA 梯度执行、flow 导数、差分对照和 CLI，不将这一检查点标记为完整 AD 完成。

## 前向 AD 执行基线：模型梯度、参数链与稀疏存储

2026 年 11 月 19 日恢复后，接通 ExecutionObserver：原 MIR 执行原值，在同一实际路径传播参数导数，phi 与活动标记均并行更新。支持出口变量及按端点方向累加的 flow 梯度；--wrt 进入 MIR-SSA 或目标切片。

数学规则覆盖当前 HIR 数值函数，增加幂下溢、tanh 饱和、atan2 缩放回归。偏置/温度网格与中心差分对齐；人工 MIR 循环验证交换 phi 的多次迭代，不扩展 VA 循环语义。

ParamEnv 增加显式覆盖来源。effective 固定其他有效参数；defaults 沿未覆盖、非种子参数的默认式传播。A→B=3A→C=B+1 在两个模式下对 A 的导数分别为 0、3；覆盖 B 会冻结该链。

新增 sparsity 分析，参数及 Value 通过单调工作队列传播 64 位种子掩码，覆盖 phi 和回边。TangentState 仅持久保存相关分量，区分 Undef 与已定义零；密集模式作为参考。掩码外的非零导数会被拒绝，不静默丢弃。

42 项 CTest 全部通过，包含 ad_rules、ad_integration、ad_parameters 和 CLI。说明在 docs/ad.md；ad-plan 导出静态种子支持和原值 SSA，尚不生成显式导数 MIR、C 内核或 OSDI。

## 梯度核验 harness

新增 validation/gradcheck，逐项比较 AD 和同参数模式下的中心差分。effective 扰动冻结其他有效值，并调用抽出的 validate_effective_parameters 重新校验范围；defaults 保持原覆盖和其余选中种子，重算其他默认式。不会混淆两种参数含义。

记录原值 MIR 分支选择，拒绝跨路径差分、范围违规、非有限结果及被浮点舍入吞掉的扰动。完成的数值不匹配输出 FAIL 报告；中断核验保留旧报告。CLI 支持 --step、--rtol、--atol。44 项 CTest 全部通过，说明见 docs/gradcheck.md。

## 向量导数 IR 与只读批量执行快照

新增 ad/ir，将加减乘除和实数转换/符号降低为局部表达式 DAG。原值 vN 与向量 d(vN) 对应；数学函数保留受检查的导数内建。校验节点参数数目、来源、类型、引用与严格的先定义后使用顺序。SSA 实际求导已执行这些生成规则，ad-plan 同时导出支持集、原值 SSA 和导数 IR。45 项测试通过。

新增 Prepared、mir::Executable 与 C++ evaluate_batch。前者保存不可变编译快照，后者在构造时复制并校验原 MIR，重复执行可跳过结构/支配重算；各次值和错误独立。defaults 模式保存非种子显式覆盖签名，防止错误复用稀疏支持；种子值和固定覆盖值可正常变化。

examples/forward_ad_demo.cpp 展示解析一次、准备一次并运行四组偏置，输出 CSV 列。47 项 CTest 全部通过，含数值、快照、批量错误隔离、数学内建和示例验收；尚未生成 C/OSDI、SIMD 或 NumPy 接口。说明在 docs/ad-ir.md 与 docs/ad-prepared.md。

## C0 生成、C ABI 与 Python 基线

新增 codegen/c，将原值 SSA、局部导数 DAG 和数学导数辅助函数生成到自包含 C11。导数使用稀疏槽，phi 并行提交，flow 按端点方向累加；保留约束、未赋值、非光滑、域和执行限额错误。仅支持 effective 参数模式与明确的 C0 子集，超范围拒绝输出。

构建实际生成 mos/math/control 三个 C 文件并用 MSVC 编译成 DLL。与 C++ 逐点核对输出及梯度，包括数学函数、参数类型/范围、条件未赋值与批量失败隔离。元信息提供数量、名称、默认有效值、参数类型和可覆盖性。

新增单点/批量 C ABI 和 Python 标准库 ctypes 封装。Python 禁止 localparam 覆盖，批量保持行顺序及逐行错误。52 项 CTest 全部通过，包含 Python 调用；文档为 docs/c-codegen.md。SIMD、NumPy、产物缓存、C 默认链和完整 BSIM/OSDI 仍未完成。

## NumPy 数组接口

使用本机已有 NumPy 2.3.5，无新增下载。新增 python/vagrad_numpy.py，将 native float64、对齐且 C 连续的输入数组直接传入 C ABI，返回样本×输出×种子雅可比。严格模式拒绝不兼容布局，copy=True 显式转换；支持输出缓冲复用并拒绝重叠。

新增数组/布局/数值/失败隔离测试和四点 NPZ 示例。53 项 CTest 通过；内核仍逐行分配工作区，未做 SIMD，也未接入优化器或产物缓存。文档为 docs/numpy.md。
