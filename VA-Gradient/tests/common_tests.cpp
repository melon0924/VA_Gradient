#include "vagrad/common/diagnostic.hpp"
#include "vagrad/common/source_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Action>
void require_out_of_range(Action action) {
    try {
        action();
    } catch (const std::out_of_range&) {
        return;
    }
    throw std::runtime_error("expected out_of_range");
}

void check_locations() {
    const vagrad::SourceFile file({0}, "mixed.va", "a\r\nbc\rd\n");
    require(file.line_count() == 4, "CRLF, CR and LF line indexing");
    const auto second = file.locate(3);
    require(second.line == 2 && second.column == 1, "second line start");
    const auto third = file.locate(6);
    require(third.line == 3 && third.column == 1, "CR line start");
    const auto end = file.locate(file.text().size());
    require(end.line == 4 && end.column == 1, "EOF after newline");
    require(file.line_text(1) == "a" && file.line_text(2) == "bc" &&
            file.line_text(3) == "d" && file.line_text(4).empty(), "source line text");
    require(file.slice(3, 5) == "bc", "half-open source slice");
    require_out_of_range([&] { file.locate(9); });
    require_out_of_range([&] { file.slice(5, 3); });
    require_out_of_range([&] { file.line_text(0); });

    const vagrad::SourceFile empty({1}, "empty.va", "");
    require(empty.line_count() == 1 && empty.locate(0).column == 1, "empty source");
    const vagrad::SourceFile utf8({2}, "utf8.va", "\xef\xbb\xbf\xe7\x94\xb5\r\nx");
    require(utf8.locate(8).line == 2 && utf8.locate(8).column == 1,
            "UTF-8 BOM and multibyte characters retain byte offsets");
}

void check_loading_and_lifetime() {
    const auto directory = std::filesystem::path(VAGRAD_TEST_DATA_DIR);
    std::shared_ptr<const vagrad::SourceFile> snapshot;
    {
        vagrad::SourceManager sources;
        const auto id = sources.load(directory / "simple_mos.va");
        const auto same = sources.load(directory / "." / "simple_mos.va");
        require(id == same && sources.file_count() == 1, "normalized path deduplication");
        snapshot = sources.get(id);
        require(snapshot->text().find("VTH0") != std::string::npos, "source contents");
        require(sources.text({id, 0, 6}) == "module", "span through manager");
        require_out_of_range([&] { sources.get(vagrad::FileId{}); });
        try {
            sources.load(directory / "file-that-does-not-exist.va");
            throw std::logic_error("missing file unexpectedly loaded");
        } catch (const std::filesystem::filesystem_error&) {
            require(sources.file_count() == 1, "failed load leaves state intact");
        }
    }
    require(snapshot->slice(0, 6) == "module", "source snapshot outlives manager");
}

void check_diagnostics() {
    vagrad::SourceManager sources;
    const auto id = sources.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va");
    const auto file = sources.get(id);
    const auto offset = file->text().find("VTH0");
    const auto location = file->locate(offset);
    std::ostringstream text;
    vagrad::print_diagnostic(text, sources, {
        vagrad::Severity::Error, "TEST_NAME", "example diagnostic",
        vagrad::SourceSpan{id, offset, offset + 4},
        {{vagrad::SourceSpan{id, 0, 6}, "declared in this module"}}
    });
    const auto prefix = vagrad::path_utf8(file->path()) + ':' + std::to_string(location.line) +
                        ':' + std::to_string(location.column);
    require(text.str().find(prefix + ": error[TEST_NAME]: example diagnostic") == 0,
            "diagnostic primary location and code");
    require(text.str().find(":1:1: note: declared in this module") != std::string::npos,
            "diagnostic related location");
}

} // namespace

int main() {
    try {
        check_locations();
        check_loading_and_lifetime();
        check_diagnostics();
        std::cout << "source positions, ownership and diagnostics passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

