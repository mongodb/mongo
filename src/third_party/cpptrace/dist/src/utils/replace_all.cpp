#include "utils/replace_all.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    void replace_all(std::string& str, string_view substr, string_view replacement) {
        std::string::size_type pos = 0;
        while((pos = str.find(substr.data(), pos, substr.size())) != std::string::npos) {
            str.replace(pos, substr.size(), replacement.data(), replacement.size());
            pos += replacement.size();
        }
    }

    void replace_all(std::string& str, const std::regex& re, string_view replacement) {
        std::smatch match;
        std::size_t i = 0;
        while(std::regex_search(str.cbegin() + i, str.cend(), match, re)) {
            str.replace(i + match.position(), match.length(), replacement.data(), replacement.size());
            i += match.position() + replacement.size();
        }
    }

    void replace_all_dynamic(std::string& str, string_view substr, string_view replacement) {
        std::string::size_type pos = 0;
        while((pos = str.find(substr.data(), pos, substr.size())) != std::string::npos) {
            str.replace(pos, substr.size(), replacement.data(), replacement.size());
            // advancing by one rather than replacement.length() in case replacement leads to
            // another replacement opportunity, e.g. folding > > > to >> > then >>>
            pos++;
        }
    }

    void replace_all_template(std::string& str, const std::pair<std::regex, string_view>& rule) {
        const auto& re = rule.first;
        const auto& replacement = rule.second;
        std::smatch match;
        std::size_t cursor = 0;
        while(std::regex_search(str.cbegin() + cursor, str.cend(), match, re)) {
            // find matching >
            const std::size_t match_begin = cursor + match.position();
            std::size_t end = match_begin + match.length();
            for(int c = 1; end < str.size() && c > 0; end++) {
                if(str[end] == '<') {
                    c++;
                } else if(str[end] == '>') {
                    c--;
                }
            }
            // make the replacement
            str.replace(match_begin, end - match_begin, replacement.data(), replacement.size());
            cursor = match_begin + replacement.size();
        }
    }
}
CPPTRACE_END_NAMESPACE
