#include "vagrad/ir/mir/mir.hpp"
#include "vagrad/frontend/parser/parser.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    using namespace vagrad;
    try {
        SourceManager s;
        const auto file=s.add_virtual("<mir-test.va>",R"(module m;
            parameter real G=1m; real ids;
            analog begin ids=G; if (1 && (G>0)) ids=1 ? G*2 : G*3; else ids=0; end
            endmodule)");
        const auto p=parse(std::make_shared<const PreprocessResult>(preprocess(s,file)));
        if(p.has_errors()) throw std::runtime_error("syntax");
        auto h=std::make_shared<const hir::HirModule>(hir::lower_hir(
            std::make_shared<const hir::BindingResult>(hir::bind_names(build_syntax_tree(p)))));
        const auto m=mir::lower_mir(h);
        if(m.has_errors() || !m.entry || m.blocks.size()!=10)
            throw std::runtime_error("if, short circuit and select must each create three blocks");
        std::size_t branches=0,returns=0,loads=0;
        std::vector<unsigned> definitions(m.values.size());
        for(const auto& b:m.blocks) {
            if(!b.terminator) throw std::runtime_error("unterminated block");
            const auto& t=*b.terminator;
            branches+=t.kind==mir::TerminatorKind::Branch;
            returns+=t.kind==mir::TerminatorKind::Return;
            for(const auto target:{t.first,t.second})
                if(target && target->value>=m.blocks.size()) throw std::runtime_error("invalid target");
            for(const auto& i:b.instructions) {
                if(i.result) ++definitions.at(i.result->value);
                loads+=i.opcode==mir::Opcode::LoadParam;
                for(const auto v:i.operands)
                    if(v.value>=m.values.size()) throw std::runtime_error("invalid value");
            }
        }
        for(const auto n:definitions) if(n!=1) throw std::runtime_error("Value must have one static definition");
        if(branches!=3 || returns!=1 || loads==0 || m.locals.size()!=3)
            throw std::runtime_error("control flow, parameter references and two hidden merge slots");
        std::cout<<"MIR-Local lowering structure passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
