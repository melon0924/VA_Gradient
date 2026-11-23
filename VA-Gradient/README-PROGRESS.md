# VA-Gradient 项目结构与进度

本文件是 `E:\OpenVAF\VA-Gradient` 的**结构与进度速览**，与详版阶段说明 [README.md](README.md) 并存。README 描述的是 H2/H3 完成时的状态（23 项 CTest），本文件覆盖到当前最新的 **MIR-Local 降低检查点（24 项 CTest）**。

| 项目 | 当前值 |
|---|---|
| 更新日期 | 2026-11-19 |
| 工程版本 | 0.1.0（CMake `project(VAGradient VERSION 0.1.0)`） |
| 实现语言 / 构建 | C++20、CMake 3.21+、MSVC 2022 Build Tools、NMake Release |
| 工程路径 | `E:\OpenVAF\VA-Gradient` |
| 构建目录 | `build/windows-msvc`（`CMakePresets.json` 的 `windows-msvc` 预设） |
| 最近一次完整验证 | **24/24 CTest 通过**（2026-11-19 10:59，见 `build/windows-msvc/Testing/Temporary/LastTest.log`） |
| 代码规模 | 生产 6,513 行（include 1,167 + src 5,012 + tools 334）、测试 2,566 行、文档 1,748 行 |
| 当前断点 | HIR → MIR-Local 降低完成；CFG、SSA、切片、自动微分、OSDI 均未开始 |

## 1. 一句话现状

前端链路已经贯通到**标量参考执行**，并在其上新增了**第一版 MIR 降低**：

```text
.va 源码
  → 预处理（宏 / include / 条件编译，带来源图）
  → Lexer（原始 token）
  → Parser（ParseEvent）
  → TreeBuilder（CST，可逐字节重建展开文本）
  → AST View（类型化字段）
  → HIR H1（作用域 / 符号 / 引用绑定）
  → HIR H2（类型、参数表、探针、贡献）
  → HIR H3（给定电位/参数/温度 → 出口值与贡献）
  → MIR-Local（基本块 + 指令 + 终结指令，CfgInfo 仅预留结构）
```

其中 H1、H2 是**静态分析层**，H3 是**参考实现层**（用来给后续优化/切片/微分做数值基准），MIR 是**可优化中间表示层**的起点。

## 2. 阶段进度总表

| 阶段 | 交付内容 | 累计 CTest | 状态 |
|---|---|---:|---|
| 1 | SourceManager / Diagnostic / SourceSpan | 3 | 完成 |
| 2 | Lexer（保留 trivia、BOM、续行、逐字节可重建） | 5 | 完成 |
| 3 | Preprocessor（对象宏/函数宏、条件编译、include、来源图） | 8 | 完成 |
| 4 | Parser（递归下降 + Pratt）、ParseEvent、TreeBuilder / CST | 12 | 完成 |
| 5 | AST View（在 CST 上的类型化字段视图，不建第二棵树） | 15 | 完成 |
| H1 | HIR 名称绑定：Scope / Symbol / Reference，读写角色 | 18 | 完成 |
| H2 | 类型化 HIR：Real/Integer/String/Net/Branch/Void/Error、参数表与 from/exclude | 23 | 基线完成 |
| H3 | 参考执行器：Block/If/Assign/Contribute、诊断收集 | 23 | 基线完成 |
| M1（第一步） | HIR → MIR-Local：指令、基本块、终结指令、可变槽 | **24** | **进行中（本检查点）** |
| M1/C1 余下 | CFG 派生、`verify_mir`、MIR/CFG 文本导出与 CLI、独立执行器、与 H3 数值对齐 | — | 未开始 |
| C2/S1 | 支配树、SSA、phi、重命名、SSA 校验 | — | 未开始 |
| 后续 | 保留控制流的切片、自动微分、真实模型回归、OSDI 后端 | — | 未开始 |

进度来源：`docs/development.md`（第 1～5 阶段与 H1～H3 的逐阶段记录）、`docs/mir-checkpoint.md`（MIR 断点）。

## 3. 目录结构

```text
VA-Gradient/
├─ CMakeLists.txt              161 行  单一静态库 vagrad_frontend + CLI + 24 项 CTest
├─ CMakePresets.json            38 行  windows-msvc 预设（NMake, Release, BUILD_TESTING=ON）
├─ README.md                   407 行  详版阶段说明（H2/H3 时的状态）
├─ README-PROGRESS.md                  本文件：结构与进度速览
├─ .gitignore                          仅忽略构建产物
├─ scripts/
│  └─ build.ps1                 49 行  vcvars64 → cmake 配置/构建 → ctest（-SkipTests 可跳过）
├─ include/vagrad/           1,167 行  接口层
│  ├─ common/                           SourceManager / SourceSpan / Diagnostic
│  ├─ frontend/
│  │  ├─ lexer/                         Token、TokenKind、Lexer 接口
│  │  ├─ preprocessor/                  宏表、include 解析、PreprocessResult
│  │  ├─ parser/                        ParseEvent、Parser 接口
│  │  ├─ syntax/                        SyntaxKind、SyntaxTree（只读 arena / NodeId）
│  │  ├─ ast/                           AST View 公共接口（ast.hpp 405 行，最大头文件）
│  │  └─ hir/                           hir.hpp(H1) / typed.hpp(H2) / eval.hpp(H3)
│  └─ ir/mir/mir.hpp            62 行  MIR-Local 数据结构
├─ src/                      5,012 行  实现层（目录与 include 一一对应）
├─ tools/
│  └─ vagrad_main.cpp          334 行  CLI 入口，8 个命令
├─ tests/                    2,566 行  9 个 C++ 测试 + 5 个 CMake 驱动的 CLI 集成测试
├─ testdata/                            验收样例（见第 6 节）
├─ docs/                     1,748 行  8 篇模块说明与实施计划
└─ build/                               编译产物、导出报告、CTest 日志
```

### 各源码目录的行数分布

| 目录 | 头文件 | 实现 | 合计 | 主要职责 |
|---|---:|---:|---:|---|
| `common/` | 115 | 153 | 268 | 路径去重、FileId、行列索引、诊断 |
| `lexer/` | 77 | 390 | 467 | 原始 token，保留空白/注释/BOM/续行 |
| `preprocessor/` | 106 | 712 | 818 | 宏展开、条件编译、include、ExpansionContext |
| `parser/` | 47 | 763 | 810 | 递归下降 + Pratt，事件与错误恢复 |
| `syntax/` | 76 | 209 | 285 | CST 只读 arena、构树校验、文本重建 |
| `ast/` | 405 | 717 | 1,122 | 类型化字段视图、AST dump |
| `hir/` | 279 | 1,910 | 2,189 | H1 绑定 / H2 类型化 / H3 执行 |
| `ir/mir/` | 62 | 158 | 220 | MIR-Local 降低 |

`hir/` 是当前最重的一块（占生产代码的 36%），其中 `binding.cpp` 611 行、`lower.cpp` 463 行、`eval.cpp` 413 行。

### 测试目录

| 测试目标 | 行数 | 覆盖对象 |
|---|---:|---|
| `ast_tests.cpp` | 366 | 字段角色、宏来源、实参复制身份、存储寿命 |
| `hir_tests.cpp` | 388 | 声明合并、作用域遮蔽、读写角色、未定义名 |
| `preprocessor_tests.cpp` | 386 | 宏/实参、条件分支、嵌套与重复 include、包含保护 |
| `parser_tests.cpp` | 350 | 优先级、事件完整性、错误恢复、CST 无损重建 |
| `lexer_tests.cpp` | 229 | 分类、数值形式、转义、逐字节原文重建 |
| `hir_typed_tests.cpp` | 203 | 类型、转换、参数依赖/覆盖/范围、探针方向 |
| `hir_eval_tests.cpp` | 149 | 六组手算参考、短路、贡献累加、执行错误 |
| `mir_tests.cpp` | 43 | 基本块/终结指令/编号/单一定义/隐藏槽 |
| `common_tests.cpp` | 110 | 换行、位置边界、快照寿命、诊断 |
| `cli_*.cmake`（5 个） | 342 | 命令衔接、配置传递、退出码、输入覆盖保护 |

## 4. 模块职责与接口

| 模块 | 接口 | 输入 → 输出 | 关键约定 |
|---|---|---|---|
| SourceManager | [source_manager.hpp](include/vagrad/common/source_manager.hpp) | 路径 → 文件快照 + FileId | 物理路径规范化去重；FileId 只在本次运行有效 |
| Lexer | [lexer.hpp](include/vagrad/frontend/lexer/lexer.hpp) | 单文件文本 → `LexResult` | 每枚非 EOF token 覆盖非空原文，范围连续 |
| Preprocessor | [preprocessor.hpp](include/vagrad/frontend/preprocessor/preprocessor.hpp) | token → `PreprocessResult` | 宏表 + 条件编译 + include 同一流程；未生效分支不加载 |
| Parser | [parser.hpp](include/vagrad/frontend/parser/parser.hpp) | 展开 token → `ParseResult`（事件） | Pratt 用 `forward_parent` 延后确认父节点 |
| TreeBuilder / CST | [syntax_tree.hpp](include/vagrad/frontend/syntax/syntax_tree.hpp) | 事件 → `SyntaxTree` | 按 token 索引补回 trivia；根文本可逐字节重建 |
| AST View | [ast.hpp](include/vagrad/frontend/ast/ast.hpp) | CST 节点 → 类型化句柄 | 共享 CST 存储，不分配第二棵树 |
| HIR H1 | [hir.hpp](include/vagrad/frontend/hir/hir.hpp) | AST View → `BindingResult` | 两遍处理：先建作用域与符号，再绑引用 |
| HIR H2 | [typed.hpp](include/vagrad/frontend/hir/typed.hpp) | AST View + H1 → `HirModule` | 参数使用处保留 Ref，不提前常量化 |
| HIR H3 | [eval.hpp](include/vagrad/frontend/hir/eval.hpp) | `HirModule` + `ParamEnv` + 输入 → `EvalResult` | 变量读前必须在本路径赋过值 |
| MIR | [mir.hpp](include/vagrad/ir/mir/mir.hpp) | `shared_ptr<const HirModule>` → `MirFunction` | 源码变量走 LoadLocal/StoreLocal，参数走 LoadParam |

### 位置与来源的两套坐标

- `SourceSpan`：原始文件的字节范围 `[begin,end)`，行/列从 1 开始，字节偏移从 0 开始。
- `Expanded[a,b)`：展开文本中的范围；宏实参被复制多次时，多个展开 token 共享同一原始位置但拥有不同节点身份。
- `Context[]`：宏/实参的来源图，记录调用位置、定义位置、`DefinitionContext` 与 `OriginContext`，是"这个节点到底来自哪一行"的唯一依据。

## 5. 当前能力边界

**已实现**

- 声明、属性、参数范围（`from`/`exclude`）、analog 块与用户函数、主要表达式、`if`/`for`/`while`/`repeat`、事件语句的**语法**层面。
- 名称绑定：端口头/方向/discipline 合并、命名块作用域、函数隐式返回变量、遮蔽。
- 类型推导：Real/Integer/String/Net/Branch/Void/Error，显式 Convert。
- 参数环境：默认值 → 覆盖 → 剩余依赖求值 → 范围校验，检测循环与未知覆盖。
- 标量参考执行：Block/If/Assign、纯数学内建、`V(a,b)` 探针、`I(br) <+ expr` 贡献，带定位诊断。
- MIR-Local 降低：`if`、`&&`、`||`、`?:` 拆基本块；表达式合流暂用隐藏 Local 槽。

**未实现（明确不在当前范围）**

- CFG 派生、`verify_mir`、MIR/CFG 文本导出与 CLI 命令（当前 CLI 只有 8 个命令，见第 7 节）。
- SSA / phi / 支配树；切片；自动微分（`ddx` 等价能力）。
- 循环、事件、用户模拟函数、状态型模拟运算的**执行**（给出能力诊断）。
- 量纲检查、完整整数位宽语义、层次/成员路径绑定。
- 完整数字 Verilog、`generate`、模块实例化、数组初始化。
- 预处理：token 粘接、宏字符串构造、默认宏实参、`__FILE__`/`__LINE__`。
- OpenVAF 后端、OSDI 生成、ngspice 仿真（本项目只做前端）。

## 6. 验收样例与统计

| 样例 | 规模 | 用途 |
|---|---|---|
| [testdata/simple_mos.va](testdata/simple_mos.va) | 58 行 / 2,018 B | 正常验收：中文注释、块注释、头文件、多行宏、嵌套宏、条件编译、属性、参数区间、branch、贡献语句 |
| [testdata/include/simple_mos_defaults.vams](testdata/include/simple_mos_defaults.vams) | 8 行 / 207 B | 默认参数宏 `DEFAULT_GAIN`、`DEFAULT_VTH` |
| [testdata/parser_errors.va](testdata/parser_errors.va) | 15 行 | 缺表达式、缺分号，验证错误恢复与退出码 1 |
| [testdata/semantic/binding_errors.va](testdata/semantic/binding_errors.va) | 12 行 | 重复 G0、未定义变量、未定义函数 |
| [testdata/semantic/typed_errors.va](testdata/semantic/typed_errors.va) | 10 行 | 类型错误样例 |
| [testdata/semantic/runtime_errors.va](testdata/semantic/runtime_errors.va) | 6 行 | 运行期错误样例 |
| [testdata/semantic/parameter_dependencies.va](testdata/semantic/parameter_dependencies.va) | 14 行 | 参数依赖、循环依赖与范围校验 |

默认样例的实测统计（与 README 第 6 节一致）：

| 指标 | 值 |
|---|---:|
| 主文件 / 头文件字节 | 2,018 / 207 |
| 展开文本字节 | 1,713 |
| 展开 token（含 trivia 与 EOF） | 486 |
| 来源上下文 | 17 |
| ParseEvent 总数 | 574 |
| CST 节点 / token 叶子 | 159 / 486 |
| 模块 / 参数声明 / 赋值 / if / 贡献语句 | 1 / 7 / 7 / 2 / 1 |
| H1 作用域 / 用户符号 / 引用 / 已解析 / 错误 | 4 / 18 / 39 / 39 / 0 |

`build/` 下的导出报告可随时重建：`simple_mos.expanded.va`、`.tokens.txt`、`.events.txt`、`.cst.txt`、`.ast.txt`、`.hir.txt`、`.typed.hir.txt`、`.eval.txt`、`.600K.eval.txt`、`parameter_dependencies.typed.hir.txt`。

## 7. CLI 命令

`tools/vagrad_main.cpp` 共 334 行，实现 8 个命令：

| 命令 | 行为 |
|---|---|
| `source <file>` | 显示路径、FileId、字节数、行索引数量 |
| `locate <file> <byte>` | 字节偏移 → 原始行列 |
| `lex <file>` | 单文件原始 token，不读 include |
| `preprocess <file> [--dump-tokens]` | 展开后的 VA 文本 / token 与来源表 |
| `parse <file> [--dump-events]` | CST / 事件序列 |
| `ast <file>` | 类型化语法视图 |
| `hir <file> [--typed]` | H1 绑定报告；`--typed` 进入 H2 并求参数有效值 |
| `eval <file> --voltage d=.05 --voltage g=1 --temperature 300` | H3 标量参考执行 |

公共选项：`-D NAME[=VALUE]`（预处理宏）、`-I DIR`（include 搜索目录）、`-o FILE`（输出）。退出码：成功 0、处理/输出错误 1、命令格式错误 2；`-o` 会校验目标不是本次加载的输入文件。

**注意**：目前没有 `mir` 或 `cfg` 命令，MIR 只能通过库接口和 `tests/mir_tests.cpp` 观察。

## 8. 最新交付：MIR-Local 检查点

见 [docs/mir-checkpoint.md](docs/mir-checkpoint.md)。本次交付的边界很明确：

**已完成**

- [mir.hpp](include/vagrad/ir/mir/mir.hpp)（62 行）：`ValueId` / `LocalId` / `BlockId`（三套独立 arena），12 种 `Opcode`（Constant、LoadParam、LoadLocal、StoreLocal、Convert、Unary、Binary、ReadPotential、Temperature、MathCall、DiagnosticCall、ContributeFlow），4 种 `TerminatorKind`（Jump、Branch、Return、Unreachable），`Instruction` 携带 `origin` / `parameter` / `probe` / `op`，`MirFunction` 持有 HIR 所有权、locals、values、blocks、entry、diagnostics。
- [lower.cpp](src/ir/mir/lower.cpp)（158 行）：`lower_mir(shared_ptr<const HirModule>)`；源码变量用 LoadLocal/StoreLocal 保留可变槽，参数用 LoadParam；`if`、`&&`、`||`、`?:` 拆成基本块并保持"只求值选中路径"；顺序语句、类型转换、数学调用、电位/温度读取、flow 贡献均有对应降低分支。
- [mir_tests.cpp](tests/mir_tests.cpp)（43 行）：检查分支块与合流块、终结指令、跳转目标编号、Value 单一定义、参数引用、隐藏合流槽。
- `scripts/build.ps1` 通过，**24/24 CTest**，原有 23 项全部保持通过。

**未完成（断点处明确声明）**

1. CFG：前驱/后继/可达性/逆后序；`CfgInfo::has_errors` 目前只有声明。
2. `verify_mir`：编号、类型、终结指令、操作数定义、支配关系校验，以及畸形 MIR 的负面测试。
3. lowering 的异常路径审计：有效 HIR 中若出现裸 Net/Branch 引用的表达式语句，`locals_.at` 可能抛异常，应在 HIR 或 MIR 层明确诊断。
4. Void 诊断调用当前多发一个未使用的常量占位。
5. MIR/CFG 文本导出与 CLI 命令。
6. 独立的基本块执行器（与 H3 不共享控制流执行方式）。
7. 与 H3 的数值对齐（默认/宏配置、六组手算点、参数覆盖、短路、贡献方向、错误状态）。
8. README、总计划、正式 MIR 说明的更新。

## 9. 后续路线

| 阶段 | 主要工作 | 验收标准 |
|---|---|---|
| M1/C1（进行中） | CFG 派生、`verify_mir`、MIR/CFG 导出、基本块执行器 | MIR 执行结果与 HIR H3 逐点一致 |
| C2/S1 | 支配树、SSA 构造、phi 插入、重命名、SSA 校验 | SSA 执行结果与 MIR/HIR 一致 |
| 切片 | 出口观察值反向切片，保留控制流 | 切片前后数值一致 |
| 微分 | 在 MIR/SSA 上做符号微分（对应 OpenVAF 的 `ddx`） | 与有限差分对照通过 |
| 真实模型 | BSIM-BULK 全前端验收（目前只做过 Lexer 逐字节重建） | 5,211 行模型可完整走通前端 |
| 后端（远期） | 代码生成、OSDI 打包、ngspice 验证 | 与 OpenVAF 产物对齐 |

完整方案见 [docs/hir-mir-cfg-plan.md](docs/hir-mir-cfg-plan.md)（464 行）。

## 10. 构建与复现

```powershell
$root = 'E:\OpenVAF\VA-Gradient'
Set-Location -LiteralPath $root

# 配置 + 构建 + 全部测试（默认使用 ..\tools\vs-buildtools 下的 vcvars64.bat）
.\scripts\build.ps1

# 只构建
.\scripts\build.ps1 -SkipTests

# 只跑测试
ctest --preset windows-msvc
```

```powershell
$exe = Join-Path $root 'build\windows-msvc\vagrad.exe'
$va  = Join-Path $root 'testdata\simple_mos.va'

& $exe ast  $va -o (Join-Path $root 'build\simple_mos.ast.txt')
& $exe hir  $va -o (Join-Path $root 'build\simple_mos.hir.txt')
& $exe hir  $va --typed -o (Join-Path $root 'build\simple_mos.typed.hir.txt')
& $exe eval $va --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300 `
                 -o (Join-Path $root 'build\simple_mos.eval.txt')

# 切换宏配置
& $exe ast $va -D SOFT_THRESHOLD -D TRACE_MOS
```

## 11. 文档索引

| 文档 | 行数 | 内容 |
|---|---:|---|
| [README.md](README.md) | 407 | 详版阶段说明（写到 H2/H3，23 项测试时的状态） |
| [docs/development.md](docs/development.md) | 133 | 第 1～5 阶段与 H1～H3 的逐阶段开发记录与接口约定 |
| [docs/mir-checkpoint.md](docs/mir-checkpoint.md) | 56 | **最新断点**：MIR-Local 已完成/未完成清单与恢复步骤 |
| [docs/hir-mir-cfg-plan.md](docs/hir-mir-cfg-plan.md) | 464 | HIR / MIR / CFG / SSA 总体实施方案 |
| [docs/hir-h1.md](docs/hir-h1.md) | 224 | H1 作用域、符号表、名称绑定 |
| [docs/hir-h2-h3.md](docs/hir-h2-h3.md) | 232 | H2 类型与参数、H3 参考执行 |
| [docs/ast-view.md](docs/ast-view.md) | 246 | AST View 接口、错误字段、生命周期 |
| [docs/parser.md](docs/parser.md) | 251 | Parser、ParseEvent、CST 构树 |
| [docs/preprocessor.md](docs/preprocessor.md) | 142 | 宏、include、条件编译与来源映射 |

**阅读顺序建议**：先第 1、2 节看链路与进度 → 第 3、4 节定位代码 → 需要改 MIR 时读 `docs/mir-checkpoint.md` 与 `docs/hir-mir-cfg-plan.md` → 其余文档按模块查阅。
