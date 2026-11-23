#pragma once

#include "vagrad/frontend/hir/typed.hpp"

namespace vagrad::mir {

// 这些 ID 分别索引不同的 arena，避免将源码变量与指令结果混用。
struct ValueId { std::size_t value; friend bool operator==(ValueId, ValueId) = default; };
struct LocalId { std::size_t value; friend bool operator==(LocalId, LocalId) = default; };
struct BlockId { std::size_t value; friend bool operator==(BlockId, BlockId) = default; };
enum class MirForm { Local, Ssa };
enum class Opcode { Constant, LoadParam, LoadLocal, StoreLocal, Convert, Unary, Binary,
                    ReadPotential, Temperature, MathCall, DiagnosticCall, ContributeFlow,
                    Undef, Phi, RequireDefined };
enum class TerminatorKind { Jump, Branch, Return, Unreachable };
struct PhiInput { BlockId predecessor; ValueId value; };
struct ReturnValue { hir::SymbolId symbol; ValueId value; };
struct Instruction {
    Opcode opcode;
    hir::Origin origin;
    std::optional<ValueId> result;
    std::vector<ValueId> operands;
    std::optional<LocalId> local;
    std::optional<hir::SymbolId> parameter;
    std::optional<hir::Probe> probe;
    hir::Scalar literal = std::int32_t{0};
    std::string op;
    std::vector<PhiInput> incoming; // phi 的值在对应前驱边上使用
};
struct Terminator {
    TerminatorKind kind;
    hir::Origin origin;
    std::optional<ValueId> condition;
    std::optional<BlockId> first, second; // Jump: first；Branch: true / false
    std::vector<ReturnValue> outputs; // SSA Return：明确的 module 变量出口值
};
struct BasicBlock {
    std::vector<Instruction> instructions;
    std::optional<Terminator> terminator;
};
struct ValueInfo { hir::Type type; hir::Origin origin; };
struct LocalInfo {
    hir::Type type;
    std::optional<hir::SymbolId> symbol; // 临时合流槽没有源码 SymbolId
    hir::Origin origin;
};
struct MirFunction {
    explicit MirFunction(std::shared_ptr<const hir::HirModule> source) : hir(std::move(source)) {}
    std::shared_ptr<const hir::HirModule> hir;
    std::vector<LocalInfo> locals;
    std::vector<ValueInfo> values;
    std::vector<BasicBlock> blocks;
    std::optional<BlockId> entry;
    std::vector<Diagnostic> diagnostics;
    MirForm form=MirForm::Local;
    // nullopt：完整输出；切片可显式缩小观察对象，空表表示不输出该类对象。
    std::optional<std::vector<hir::SymbolId>> output_projection,flow_projection;
    bool has_errors() const;
};
struct CfgInfo {
    std::vector<std::vector<BlockId>> predecessors, successors;
    std::vector<bool> reachable;
    std::vector<BlockId> reverse_postorder;
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const;
};

MirFunction lower_mir(std::shared_ptr<const hir::HirModule>);
MirFunction promote_to_ssa(const MirFunction&);
CfgInfo build_cfg(const MirFunction&);
// 编辑 MIR 跳转后必须重建 CfgInfo；它不独立维护另一份权威边表。
std::vector<Diagnostic> verify_mir(const MirFunction&);
void print_mir(std::ostream&, const MirFunction&);
void print_cfg(std::ostream&, const MirFunction&, const CfgInfo&);
std::string_view opcode_name(Opcode);

} // namespace vagrad::mir
