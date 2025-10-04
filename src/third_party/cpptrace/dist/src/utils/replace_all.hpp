#ifndef REPLACE_ALL
#define REPLACE_ALL

#include <string>
#include <regex>

#include "utils/string_view.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    // replace all instances of substr with the replacement
    void replace_all(std::string& str, string_view substr, string_view replacement);

    // replace all regex matches with the replacement
    void replace_all(std::string& str, const std::regex& re, string_view replacement);

    // replace all instances of substr with the replacement, including new instances introduced by the replacement
    void replace_all_dynamic(std::string& str, string_view substr, string_view replacement);

    // replace all matches of a regex including template parameters
    void replace_all_template(std::string& str, const std::pair<std::regex, string_view>& rule);
}
CPPTRACE_END_NAMESPACE

#endif
