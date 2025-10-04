#include "snippets/snippet.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <iostream>

#include "utils/common.hpp"
#include "utils/microfmt.hpp"
#include "utils/utils.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    constexpr std::int64_t max_size = 1024 * 1024 * 10; // 10 MiB

    struct line_range {
        std::size_t begin;
        std::size_t end; // one past the end
    };

    class snippet_manager {
        bool loaded_contents;
        std::string contents;
        // 1-based indexing
        std::vector<line_range> line_table;
    public:
        snippet_manager(const std::string& path) : loaded_contents(false) {
            std::ifstream file;
            try {
                file.open(path, std::ios::ate);
                if(file.is_open()) {
                    std::ifstream::pos_type size = file.tellg();
                    if(size == std::ifstream::pos_type(-1) || size > max_size) {
                        return;
                    }
                    // else load file
                    file.seekg(0, std::ios::beg);
                    contents.resize(to<std::size_t>(size));
                    if(!file.read(&contents[0], size)) {
                        // error ...
                    }
                    build_line_table();
                    loaded_contents = true;
                }
            } catch(const std::ifstream::failure&) {
                // ...
            }
        }

        // takes a 1-index line
        std::string get_line(std::size_t line) const {
            if(!loaded_contents || line > line_table.size()) {
                return "";
            } else {
                return contents.substr(line_table[line].begin, line_table[line].end - line_table[line].begin);
            }
        }

        std::size_t num_lines() const {
            return line_table.size();
        }

        bool ok() const {
            return loaded_contents;
        }
    private:
        void build_line_table() {
            line_table.push_back({0, 0});
            std::size_t pos = 0; // stores the start of the current line
            while(true) {
                // find the end of the current line
                std::size_t terminator_pos = contents.find('\n', pos);
                if(terminator_pos == std::string::npos) {
                    line_table.push_back({pos, contents.size()});
                    break;
                } else {
                    std::size_t end_pos = terminator_pos; // one past the end of the current line
                    if(end_pos > 0 && contents[end_pos - 1] == '\r') {
                        end_pos--;
                    }
                    line_table.push_back({pos, end_pos});
                    pos = terminator_pos + 1;
                }
            }
        }
    };

    const snippet_manager& get_manager(const std::string& path) {
        static std::mutex snippet_manager_mutex;
        static std::unordered_map<std::string, const snippet_manager> snippet_managers;
        std::unique_lock<std::mutex> lock(snippet_manager_mutex);
        auto it = snippet_managers.find(path);
        if(it == snippet_managers.end()) {
            return snippet_managers.insert({path, snippet_manager(path)}).first->second;
        } else {
            return it->second;
        }
    }

    // how wide the margin for the line number should be
    constexpr std::size_t margin_width = 8;

    struct snippet_context {
        std::size_t original_begin;
        std::size_t begin;
        std::size_t end;
        std::vector<std::string> lines;
    };

    optional<snippet_context> get_lines(const std::string& path, std::size_t target_line, std::size_t context_size) {
        const auto& manager = get_manager(path);
        if(!manager.ok()) {
            return nullopt;
        }
        auto begin = target_line <= context_size + 1 ? 1 : target_line - context_size;
        auto original_begin = begin;
        auto end = std::min(target_line + context_size, manager.num_lines() - 1);
        std::vector<std::string> lines;
        for(auto line = begin; line <= end; line++) {
            lines.push_back(manager.get_line(line));
        }
        // trim blank lines
        while(begin < target_line && lines[begin - original_begin].empty()) {
            begin++;
        }
        while(end > target_line && lines[end - original_begin].empty()) {
            end--;
        }
        return snippet_context{original_begin, begin, end, std::move(lines)};
    }

    std::string write_line_number(std::size_t line, std::size_t target_line, bool color) {
        std::string snippet;
        auto line_str = std::to_string(line);
        if(line == target_line) {
            if(color) {
                snippet += YELLOW;
            }
            auto line_width = line_str.size() + 3;
            snippet += microfmt::format(
                "{>{}} > {}: ",
                line_width > margin_width ? 0 : margin_width - line_width,
                "",
                line_str
            );
            if(color) {
                snippet += RESET;
            }
        } else {
            snippet += microfmt::format("{>{}}: ", margin_width, line_str);
        }
        return snippet;
    }

    std::string write_carrot(std::uint32_t column, bool color) {
        std::string snippet;
        snippet += microfmt::format("\n{>{}}", margin_width + 2 + column - 1, "");
        if(color) {
            snippet += YELLOW;
        }
        snippet += "^";
        if(color) {
            snippet += RESET;
        }
        return snippet;
    }

    std::string write_line(
        std::size_t line,
        nullable<std::uint32_t> column,
        std::size_t target_line,
        bool color,
        const snippet_context& context
    ) {
        std::string snippet;
        snippet += write_line_number(line, target_line, color);
        snippet += context.lines[line - context.original_begin];
        if(line == target_line && column.has_value()) {
            snippet += write_carrot(column.value(), color);
        }
        return snippet;
    }

    // 1-indexed line
    std::string get_snippet(
        const std::string& path,
        std::size_t target_line,
        nullable<std::uint32_t> column,
        std::size_t context_size,
        bool color
    ) {
        const auto context_res = get_lines(path, target_line, context_size);
        if(!context_res) {
            return "";
        }
        const auto& context = context_res.unwrap();
        std::string snippet;
        for(auto line = context.begin; line <= context.end; line++) {
            snippet += write_line(line, column, target_line, color, context);
            if(line != context.end) {
                snippet += '\n';
            }
        }
        return snippet;
    }
}
CPPTRACE_END_NAMESPACE
