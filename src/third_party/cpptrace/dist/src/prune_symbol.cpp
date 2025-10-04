#include "cpptrace/forward.hpp"

#include <array>
#include <cctype>
#include <vector>

#include "utils/error.hpp"
#include "utils/optional.hpp"
#include "utils/string_view.hpp"
#include "utils/utils.hpp"

// Docs
// https://itanium-cxx-abi.github.io/cxx-abi/abi.html
// https://en.wikiversity.org/wiki/Visual_C%2B%2B_name_mangling

// Demangling
// https://github.com/llvm/llvm-project/blob/main/libcxxabi/src/demangle/ItaniumDemangle.h
// https://github.com/gcc-mirror/gcc/blob/b76f1fb7bf8a7b66b8acd469309257f8b18c0c51/libiberty/cp-demangle.c#L6794
// https://github.com/wine-mirror/wine/blob/3295365ba5654d6ff2da37c1ffa84aed81291fc1/dlls/msvcrt/undname.c#L1476

// Mangling
// https://github.com/llvm/llvm-project/blob/1463da8c4063cf1f1513aa5dbcedb44d2099c87f/clang/include/clang/AST/Mangle.h
// https://github.com/llvm/llvm-project/blob/1463da8c4063cf1f1513aa5dbcedb44d2099c87f/clang/lib/AST/MicrosoftMangle.cpp#L1709-L1721

// Test cases
// https://github.com/llvm/llvm-project/tree/d1b0b4bb4405c144e23be3d5c0459b03f95bd5ac/llvm/test/Demangle
// https://github.com/llvm/llvm-project/blob/d1b0b4bb4405c144e23be3d5c0459b03f95bd5ac/libcxxabi/test/DemangleTestCases.inc
// https://github.com/llvm/llvm-project/blob/d1b0b4bb4405c144e23be3d5c0459b03f95bd5ac/libcxxabi/test/test_demangle.pass.cpp
// https://github.com/wine-mirror/wine/blob/3295365ba5654d6ff2da37c1ffa84aed81291fc1/dlls/msvcrt/tests/cpp.c#L108
// https://github.com/wine-mirror/wine/blob/3295365ba5654d6ff2da37c1ffa84aed81291fc1/dlls/ucrtbase/tests/cpp.c#L57


CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    template<typename T, typename Arg>
    bool is_any(const T& value, const Arg& arg) {
        return value == arg;
    }

    template<typename T, typename Arg, typename... Args>
    bool is_any(const T& value, const Arg& arg, const Args&... args) {
        return (value == arg) || is_any(value, args...);
    }

    // http://eel.is/c++draft/lex.name#nt:identifier
    bool is_identifier_start(char c) {
        return isalpha(c) || c == '$' || c == '_';
    }
    bool is_identifier_continue(char c) {
        return isdigit(c) || is_identifier_start(c);
    }
    bool is_hex_digit(char c) {
        return isdigit(c) || is_any(c, 'a', 'b', 'c', 'd', 'e', 'f', 'A', 'B', 'C', 'D', 'E', 'F');
    }
    bool is_octal_digit(char c) {
        return is_any(c, '0', '1', '2', '3', '4', '5', '6', '7');
    }
    bool is_simple_escape_char(char c) {
        return is_any(c, '\'', '"', '?', '\\', 'a', 'b', 'f', 'n', 'r', 't', 'v');
    }

    // http://eel.is/c++draft/lex.operators#nt:operator-or-punctuator
    const std::vector<string_view> punctuators_and_operators = []() {
        std::vector<string_view> vec{
            "{",        "}",        "[",        "]",        "(",        ")",
            "<:",       ":>",       "<%",       "%>",       ";",        ":",        "...",
            "?",        "::",       ".",        ".*",       "->",       "->*",      "~",
            "!",        "+",        "-",        "*",        "/",        "%",        "^",        "&",        "|",
            "=",        "+=",       "-=",       "*=",       "/=",       "%=",       "^=",       "&=",       "|=",
            "==",       "!=",       "<",        ">",        "<=",       ">=",       "<=>",      "&&",       "||",
            "<<",       ">>",       "<<=",      ">>=",      "++",       "--",       ",",
            // "and",      "or",       "xor",      "not",      "bitand",   "bitor",    "compl",
            // "and_eq",   "or_eq",    "xor_eq",   "not_eq",
            "#", // extension for {lambda()#1}
        };
        std::sort(vec.begin(), vec.end(), [](string_view a, string_view b) { return a.size() > b.size(); });
        return vec;
    } ();

    const std::array<string_view, 2> anonymous_namespace_spellings = {"(anonymous namespace)", "`anonymous namespace'"};

    bool is_opening_punctuation(string_view token) {
        return token == "(" || token == "[" || token == "{" || token == "<";
    }

    bool is_closing_punctuation(string_view token) {
        return token == ")" || token == "]" || token == "}" || token == ">";
    }

    string_view get_corresponding_punctuation(string_view token) {
        if(token == "(") {
            return ")";
        } else if(token == "[") {
            return "]";
        } else if(token == "{") {
            return "}";
        } else if(token == "<") {
            return ">";
        }
        PANIC();
    }

    bool is_ignored_identifier(string_view string) {
        return is_any(string, "const", "volatile", "decltype", "noexcept");
    }

    bool is_microsoft_calling_convention(string_view string) {
        return is_any(
            string,
            "__cdecl",
            "__clrcall",
            "__stdcall",
            "__fastcall",
            "__thiscall",
            "__vectorcall"
        );
    }

    // There are five kinds of tokens in C++: identifiers, keywords, literals, operators, and other separators
    // We tokenize a mostly-subset of this:
    //  - identifiers/keywords
    //  - literals: char, string, int, float. Msvc `strings' too.
    //  - punctuation
    // Additionally we tokenize a few things that are useful
    //  - anonymous namespace tags

    enum class token_type {
        identifier,
        punctuation,
        literal,
        anonymous_namespace
    };

    struct token {
        token_type type;
        string_view str;

        bool operator==(const token& other) const {
            return type == other.type && str == other.str;
        }
    };

    bool is_pointer_ref(const token& token) {
        return token.type == token_type::punctuation && is_any(token.str, "*", "&", "&&");
    }

    struct parse_error {
        int x; // this works around a gcc bug with warn_unused_result and empty structs
        explicit parse_error() = default;
        string_view what() const {
            return "Parse error";
        }
    };

    #define CONCAT_IMPL(X, Y) X##Y
    #define CONCAT(X, Y) CONCAT_IMPL(X, Y)
    #define UNIQUE(X) CONCAT(X, __COUNTER__)
    #define TRY_PARSE_IMPL(ACTION, SUCCESS, RES) \
        Result<bool, parse_error> RES = (ACTION); \
        if((RES).is_error()) { \
            return std::move((RES)).unwrap_error(); \
        } else if((RES).unwrap_value()) { \
            SUCCESS; \
        }
    #define TRY_PARSE(ACTION, SUCCESS) TRY_PARSE_IMPL(ACTION, SUCCESS, UNIQUE(res))

    #define TRY_TOK_IMPL(RES, ACTION, TMP) \
        const auto TMP = (ACTION); \
        if((TMP).is_error()) { \
            return std::move((TMP)).unwrap_error(); \
        } \
        const auto RES = std::move((TMP)).unwrap_value()
    #define TRY_TOK(RES, ACTION) TRY_TOK_IMPL(RES, ACTION, UNIQUE(tmp))

    class symbol_tokenizer {
    private:
        string_view source;
        optional<token> next_token;

        bool peek(string_view text, size_t pos = 0) const {
            return text == source.substr(pos, text.size());
        }

        NODISCARD Result<optional<token>, parse_error> peek_anonymous_namespace() const {
            for(const auto& spelling : anonymous_namespace_spellings) {
                if(peek(spelling)) {
                    return token{token_type::anonymous_namespace, {source.begin(), spelling.size()}};
                }
            }
            return nullopt;
        }

        NODISCARD Result<optional<token>, parse_error> peek_number() const {
            // More or less following pp-number https://eel.is/c++draft/lex.ppnumber
            auto cursor = source.begin();
            if(cursor != source.end() && std::isdigit(*cursor)) {
                while(
                    cursor != source.end()
                    && (
                        std::isdigit(*cursor)
                        || is_identifier_continue(*cursor)
                        || is_any(*cursor, '\'', '-', '+', '.')
                    )
                ) {
                    cursor++;
                }
            }
            if(cursor == source.begin()) {
                return nullopt;
            }
            return token{token_type::literal, {source.begin(), cursor}};
        }

        NODISCARD Result<optional<token>, parse_error> peek_msvc_string() const {
            // msvc strings look like `this'
            // they nest, e.g.: ``int main(void)'::`2'::<lambda_1>::operator()(void)const'
            // TODO: Escapes?
            auto cursor = source.begin();
            if(cursor != source.end() && *cursor == '`') {
                int depth = 0;
                do {
                    if(*cursor == '`') {
                        depth++;
                    } else if(*cursor == '\'') {
                        depth--;
                    }
                    cursor++;
                } while(cursor != source.end() && depth != 0);
                if(depth != 0) {
                    return parse_error{};
                }
            }
            if(cursor == source.begin()) {
                return nullopt;
            }
            return token{token_type::literal, {source.begin(), cursor}};
        }

        NODISCARD Result<optional<token>, parse_error> parse_quoted_string() const {
            auto cursor = source.begin();
            if(cursor != source.end() && is_any(*cursor, '\'', '"')) {
                auto closing_quote = *cursor;
                cursor++;
                while(cursor != source.end() && *cursor != closing_quote) {
                    if(*cursor == '\\') {
                        if(cursor + 1 == source.end()) {
                            return parse_error{};
                        }
                        cursor += 2;
                    }
                    cursor++;
                }
                if(cursor == source.end() || *cursor != closing_quote) {
                    return parse_error{};
                }
                cursor++;
            }
            if(cursor == source.begin()) {
                return nullopt;
            }
            return token{token_type::literal, {source.begin(), cursor}};
        }

        NODISCARD Result<optional<token>, parse_error> peek_literal() const {
            TRY_TOK(number, peek_number());
            if(number) {
                return number;
            }
            TRY_TOK(msvc_string, peek_msvc_string());
            if(msvc_string) {
                return msvc_string;
            }
            TRY_TOK(quoted_string, parse_quoted_string());
            if(quoted_string) {
                return quoted_string;
            }
            return nullopt;
        }

        NODISCARD Result<optional<token>, parse_error> peek_punctuation(size_t pos = 0) const {
            for(const auto punctuation : punctuators_and_operators) {
                if(peek(punctuation, pos)) {
                    return token{token_type::punctuation, {source.begin() + pos, punctuation.size()}};
                }
            }
            return nullopt;
        }

        NODISCARD Result<optional<token>, parse_error> peek_identifier(size_t pos = 0) const {
            auto start = source.begin() + std::min(pos, source.size());;
            auto cursor = start;
            if(cursor != source.end() && is_identifier_start(*cursor)) {
                while(cursor != source.end() && is_identifier_continue(*cursor)) {
                    cursor++;
                }
            }

            if(cursor == start) {
                return nullopt;
            }
            return token{token_type::identifier, {start, cursor}};
        }

        NODISCARD token peek_misc() const {
            ASSERT(!source.empty());
            return token{token_type::punctuation, {source.begin(), 1}};
        }

        Result<monostate, parse_error> maybe_load_next_token() {
            if(next_token.has_value()) {
                return monostate{};
            }
            while(!source.empty() && std::isspace(source[0])) {
                source.advance(1);
            }
            if(source.empty()) {
                return monostate{};
            }
            TRY_TOK(anon, peek_anonymous_namespace());
            if(anon) {
                next_token = anon.unwrap();
                return monostate{};
            }
            TRY_TOK(literal, peek_literal());
            if(literal) {
                next_token = literal.unwrap();
                return monostate{};
            }
            TRY_TOK(punctuation, peek_punctuation());
            if(punctuation) {
                next_token = punctuation.unwrap();
                return monostate{};
            }
            TRY_TOK(identifier, peek_identifier());
            if(identifier) {
                next_token = identifier.unwrap();
                return monostate{};
            }
            next_token = peek_misc();
            return monostate{};
        }

        optional<token> get_adjusted_next_token(bool in_template_argument_list) {
            // https://eel.is/c++draft/temp.names#4 decompose >> to > when we think we're in a template argument list.
            // We don't have to do this for >>= or >=.
            if(next_token && in_template_argument_list && next_token.unwrap() == token{token_type::punctuation, ">>"}) {
                auto copy = next_token.unwrap();
                copy.str = copy.str.substr(0, 1); // ">"
                return copy;
            }
            return next_token;
        }

    public:
        symbol_tokenizer(string_view source) : source(source) {}

        NODISCARD Result<optional<token>, parse_error> peek(bool in_template_argument_list = false) {
            auto res = maybe_load_next_token();
            if(res.is_error()) {
                return res.unwrap_error();
            }
            return get_adjusted_next_token(in_template_argument_list);
        }

        Result<optional<token>, parse_error> advance(bool in_template_argument_list = false) {
            TRY_TOK(next, peek(in_template_argument_list));
            if(!next) {
                return nullopt;
            }
            source.advance(next.unwrap().str.size());
            next_token.reset();
            return next;
        }

        NODISCARD Result<optional<token>, parse_error> accept(token_type type, bool in_template_argument_list = false) {
            TRY_TOK(next, peek(in_template_argument_list));
            if(next && next.unwrap().type == type) {
                advance();
                return next;
            }
            return nullopt;
        }

        NODISCARD Result<optional<token>, parse_error> accept(token token, bool in_template_argument_list = false) {
            TRY_TOK(next, peek(in_template_argument_list));
            if(next && next.unwrap() == token) {
                advance();
                return next;
            }
            return nullopt;
        }
    };

    std::string prune_symbol(string_view symbol);

    /*

    Approximate grammar, very hacky:

    full-symbol := { symbol }

    symbol := symbol-fragment { "::" ["*"] symbol-fragment }

    symbol-fragment := symbol-base [ pointer-refs ] [ punctuation-and-trailing-modifiers ]

    symbol-base := symbol-term function-pointer
                 | symbol-term
                 | function-pointer

    punctuation-and-trailing-modifiers := { [ balanced-punctuation ] [ ignored-identifier ] [ pointer-refs ] }

    symbol-term := anonymous-namespace
                 | operator
                 | name
                 | lambda
                 | unnamed

    function-pointer := "(" pointer-refs-junk symbol ")"

    pointer-refs-junk := { function-pointer-modifier } { pointer-refs } { ignored-identifier }

    anonymous-namespace := "(anonymous namespace)" | "`anonymous namespace'"

    operator := "operator" operator-type

    operator-type := new-delete
                   | special-decltype
                   | "co_await"
                   | "\"\"" IDENTIFIER
                   | OPERATOR
                   | matching-punctuation
                   | conversion-operator

    new-delete := ( "new" | "delete" ) [ "[" "]" ]

    special-decltype := "decltype" "(" ( "auto" | "nullptr" ) ")"

    matching-punctuation := "(" ")"
                          | "[" "]"

    conversion-operator := symbol [ symbol ] // kind of a hack

    name := [ "~" ] IDENTIFIER

    lambda := "{" "lambda" { PUNCTUATION } "#" LITERAL "}"
            | LITERAL                          // 'lambda*' or `symbol'
            | "<" IDENTIFIER ">"               // lambda_*

    unnamed := "{" "unnamed" "#" LITERAL "}"
             | LITERAL                         // 'unnamed*'

    balanced-punctuation := "(" balanced-punctuation-innards ")"
                          | "[" balanced-punctuation-innards "]"
                          | "<" balanced-punctuation-innards ">"
                          | "{" balanced-punctuation-innards "}"

    balanced-punctuation-innards := { ANY-TOKEN } | balanced-punctuation

    pointer-refs := { "*" | "&" | "&&" }

    ignored-identifier := "const" | "volatile" | "decltype" | "noexcept"

    function-pointer-modifier := "__cdecl" | "__clrcall" | "__stdcall" | "__fastcall" | "__thiscall" | "__vectorcall"

    */

    class symbol_parser {
        symbol_tokenizer& tokenizer;
        std::string name_output;
        bool last_was_identifier = false;
        bool reset_output_flag = false;

        void append_output(token token) {
            auto is_identifier = token.type == token_type::identifier;
            if(reset_output_flag) {
                name_output.clear();
                reset_output_flag = false;
                last_was_identifier = false;
            } else if(is_identifier && last_was_identifier) {
                name_output += ' ';
            }
            name_output += token.str;
            last_was_identifier = is_identifier;
        }

        NODISCARD Result<bool, parse_error> accept_pointer_ref(bool append) {
            bool matched = false;
            while(true) {
                TRY_TOK(token, tokenizer.peek());
                if(!token || !is_pointer_ref(token.unwrap())) {
                    break;
                }
                matched = true;
                if(append) {
                    append_output(token.unwrap());
                }
                tokenizer.advance();
            }
            return matched;
        }

        NODISCARD Result<bool, parse_error> consume_balanced(string_view opening_punctuation) {
            // priority means only consider this punctuation type and no priority means considers each independently
            bool is_in_template_list = opening_punctuation == "<";
            int depth = 1;
            auto closing_punctuation = get_corresponding_punctuation(opening_punctuation);
            while(depth > 0) {
                // first try to accept an operator, this is for things like void foo<&S::operator>(S const&)>()::test
                if(is_in_template_list) {
                    TRY_PARSE(accept_operator(true), continue);
                }
                // otherwise handle arbitrary token
                TRY_TOK(next, tokenizer.advance(is_in_template_list));
                if(!next) {
                    break;
                }
                if(next.unwrap().type == token_type::punctuation) {
                    if(next.unwrap().str == opening_punctuation) {
                        depth++;
                    } else if(next.unwrap().str == closing_punctuation) {
                        depth--;
                    } else if(is_in_template_list && is_opening_punctuation(next.unwrap().str)) {
                        // If we're in a template list, recurse into any balanced punctuation we see. This handles cases
                        // like foo<(S > S)>
                        TRY_PARSE(consume_balanced(next.unwrap().str), (void)0);
                    }
                }
            }
            return true;
        }

        NODISCARD Result<bool, parse_error> accept_balanced_punctuation() {
            TRY_TOK(token, tokenizer.peek());
            if(token && token.unwrap().type == token_type::punctuation && is_opening_punctuation(token.unwrap().str)) {
                tokenizer.advance();
                TRY_PARSE(consume_balanced(token.unwrap().str), (void)0);
                return true;
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> accept_ignored_identifier() {
            TRY_TOK(token, tokenizer.peek());
            if(token && token.unwrap().type == token_type::identifier && is_ignored_identifier(token.unwrap().str)) {
                tokenizer.advance();
                return true;
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> accept_anonymous_namespace() {
            TRY_TOK(token, tokenizer.accept(token_type::anonymous_namespace));
            if(token) {
                append_output({ token_type::identifier, "(anonymous namespace)" });
                return true;
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> accept_new_delete() {
            TRY_TOK(token, tokenizer.peek());
            if(token && token.unwrap().type == token_type::identifier && is_any(token.unwrap().str, "new", "delete")) {
                tokenizer.advance();
                append_output(token.unwrap());
                TRY_TOK(op, tokenizer.accept({token_type::punctuation, "["}));
                if(op) {
                    append_output(op.unwrap());
                    TRY_TOK(op2, tokenizer.accept({token_type::punctuation, "]"}));
                    if(!op2) {
                        return parse_error{};
                    }
                    append_output(op2.unwrap());
                }
                return true;
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> accept_special_decltype() {
            TRY_TOK(token, tokenizer.accept({token_type::identifier, "decltype"}));
            if(token) {
                append_output(token.unwrap());
                TRY_TOK(op, tokenizer.accept({token_type::punctuation, "("}));
                if(!op) {
                    return parse_error{};
                }
                append_output(op.unwrap());
                TRY_TOK(ident, tokenizer.accept(token_type::identifier));
                if(!ident) {
                    return parse_error{};
                }
                if(!is_any(ident.unwrap().str, "auto", "nullptr")) {
                    return parse_error{};
                }
                append_output(ident.unwrap());
                TRY_TOK(op2, tokenizer.accept({token_type::punctuation, ")"}));
                if(!op2) {
                    return parse_error{};
                }
                append_output(op2.unwrap());
                return true;
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> accept_operator(bool is_in_template_list = false) {
            // If we're in a template argument list we skip new/delete/auto/co_await/literal/conversion and only handle
            // the operator punctuation case. We also don't append for template lists.
            TRY_TOK(token, tokenizer.accept({token_type::identifier, "operator"}));
            if(token) {
                if(!is_in_template_list) {
                    append_output(token.unwrap());
                    TRY_PARSE(accept_new_delete(), return true);
                    TRY_PARSE(accept_special_decltype(), return true);
                    TRY_TOK(coawait, tokenizer.accept({token_type::identifier, "co_await"}));
                    if(coawait) {
                        append_output(coawait.unwrap());
                        return true;
                    }
                    TRY_TOK(literal, tokenizer.accept({token_type::literal, "\"\""}));
                    if(literal) {
                        TRY_TOK(name, tokenizer.accept(token_type::identifier));
                        if(!name) {
                            return parse_error{};
                        }
                        append_output(literal.unwrap());
                        append_output(name.unwrap());
                        return true;
                    }
                }
                TRY_TOK(op, tokenizer.accept(token_type::punctuation));
                if(op) {
                    if(!is_in_template_list) {
                        append_output(op.unwrap());
                    }
                    if(is_any(op.unwrap().str, "(", "[")) {
                        TRY_TOK(op2, tokenizer.accept(token_type::punctuation));
                        if(!op2 || op2.unwrap().str != get_corresponding_punctuation(op.unwrap().str)) {
                            return parse_error{};
                        }
                        if(!is_in_template_list) {
                            append_output(op2.unwrap());
                        }
                    }
                    return true;
                }
                if(!is_in_template_list) {
                    // Otherwise try to parse a symbol, in the case of a conversion operator
                    // There is a bit of a grammer hack here, it doesn't properly "nest," but it works
                    TRY_PARSE(parse_symbol(), (void)0);
                    // In the case of a member function pointer, there will be a type symbol followed by a symbol for
                    // the type that the member pointer points to
                    TRY_TOK(maybe_ident, tokenizer.peek());
                    if(maybe_ident && maybe_ident.unwrap().type == token_type::identifier) {
                        TRY_PARSE(parse_symbol(), (void)0);
                    }
                    return true;
                }
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> accept_identifier_token() {
            bool expect = false;
            TRY_TOK(complement, tokenizer.accept({token_type::punctuation, "~"}));
            if(complement) {
                append_output(complement.unwrap());
                expect = true;
            }
            TRY_TOK(token, tokenizer.accept(token_type::identifier));
            if(token) {
                append_output(token.unwrap());
                return true;
            } else if(expect) {
                return parse_error{};
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> consume_punctuation() {
            bool did_consume = false;
            while(true) {
                TRY_TOK(token, tokenizer.peek());
                if(
                    !token
                    || !(
                        token.unwrap().type == token_type::punctuation
                        && token.unwrap().str != "::"
                        && token.unwrap().str != "#"
                    )
                ) {
                    break;
                }
                TRY_PARSE(accept_balanced_punctuation(), {did_consume = true; continue;});
                // otherwise, if not balanced punctuation, just consume and drop
                tokenizer.advance();
                did_consume = true;
            }
            return did_consume;
        }

        NODISCARD Result<bool, parse_error> accept_lambda_or_unnamed() {
            // LLVM does main::'lambda'<...>(...)::operator()<...>(...) -- apparently this can be 'lambda<count>'
            // GCC does main::{lambda<...>(...)#1}::operator()<...>(...)
            // MSVC does `int main(void)'::`2'::<lambda_1>::operator()<...>(...)
            // https://github.com/llvm/llvm-project/blob/90beda2aba3cac34052827c560449fcb184c7313/libcxxabi/src/demangle/ItaniumDemangle.h#L1848-L1850 TODO: What about the count?
            // https://github.com/gcc-mirror/gcc/blob/b76f1fb7bf8a7b66b8acd469309257f8b18c0c51/libiberty/cp-demangle.c#L6210-L6251 TODO: What special characters can appear?
            TRY_TOK(opening_brace, tokenizer.accept({token_type::punctuation, "{"}));
            if(opening_brace) {
                token token1{};
                token token2{};
                bool two_tokens = false; // this awfulness to work around gcc's maybe-uninitialized analysis
                TRY_TOK(lambda_token, tokenizer.accept({token_type::identifier, "lambda"}));
                if(lambda_token) {
                    token1 = lambda_token.unwrap();
                } else {
                    TRY_TOK(unnamed_token, tokenizer.accept({token_type::identifier, "unnamed"}));
                    if(!unnamed_token) {
                        return parse_error{};
                    }
                    TRY_TOK(type_token, tokenizer.accept({token_type::identifier, "type"}));
                    if(!type_token) {
                        return parse_error{};
                    }
                    token1 = unnamed_token.unwrap();
                    token2 = type_token.unwrap();
                    two_tokens = true;
                }
                TRY_PARSE(consume_punctuation(), (void)0);
                TRY_TOK(hash_token, tokenizer.accept({token_type::punctuation, "#"}));
                if(!hash_token) {
                    return parse_error{};
                }
                TRY_TOK(discriminator_token, tokenizer.accept(token_type::literal));
                if(!discriminator_token) {
                    return parse_error{};
                }
                TRY_TOK(closing_brace, tokenizer.accept({token_type::punctuation, "}"}));
                if(!closing_brace) {
                    return parse_error{};
                }
                append_output({token_type::punctuation, "<"});
                append_output(token1);
                if(two_tokens) {
                    append_output(token2);
                }
                append_output(hash_token.unwrap());
                append_output(discriminator_token.unwrap());
                append_output({token_type::punctuation, ">"});
                return true;
            }
            TRY_TOK(maybe_literal_token, tokenizer.peek());
            if(
                maybe_literal_token
                && maybe_literal_token.unwrap().type == token_type::literal
                && (
                    maybe_literal_token.unwrap().str.starts_with("'lambda")
                    || maybe_literal_token.unwrap().str.starts_with("'unnamed")
                )
                && maybe_literal_token.unwrap().str.ends_with("'")
            ) {
                tokenizer.advance();
                append_output({token_type::punctuation, "<"});
                auto str = maybe_literal_token.unwrap().str;
                append_output({token_type::punctuation, str.substr(1, str.size() - 2)});
                append_output({token_type::punctuation, ">"});
                return true;
            }
            if(
                maybe_literal_token
                && maybe_literal_token.unwrap().type == token_type::literal
                && maybe_literal_token.unwrap().str.starts_with("`")
                && maybe_literal_token.unwrap().str.ends_with("'")
            ) {
                tokenizer.advance();
                // append_output(maybe_literal_token.unwrap());
                // This string is going to be another symbol, recursively reduce it
                append_output({token_type::punctuation, "`"});
                auto symbol = maybe_literal_token.unwrap().str;
                ASSERT(symbol.size() >= 2);
                auto name = detail::prune_symbol(symbol.substr(1, symbol.size() - 2));
                append_output({token_type::literal, name});
                append_output({token_type::punctuation, "'"});
                return true;
            }
            TRY_TOK(opening_bracket, tokenizer.accept({token_type::punctuation, "<"}));
            if(opening_bracket) {
                TRY_TOK(lambda_token, tokenizer.accept(token_type::identifier));
                if(!lambda_token || !lambda_token.unwrap().str.starts_with("lambda_")) {
                    return parse_error{};
                }
                TRY_TOK(closing_bracket, tokenizer.accept({token_type::punctuation, ">"}));
                if(!closing_bracket) {
                    return parse_error{};
                }
                append_output(opening_bracket.unwrap());
                append_output(lambda_token.unwrap());
                append_output(closing_bracket.unwrap());
                return true;
            }
            return false;
        }

        // lookahead to match "(*", "(__cdecl*", etc
        NODISCARD bool lookahead_is_function_pointer() const {
            auto res = [] (symbol_tokenizer tokenizer_copy) -> Result<bool, parse_error> {
                TRY_TOK(maybe_opening_parenthesis, tokenizer_copy.accept({token_type::punctuation, "("}));
                if(maybe_opening_parenthesis) {
                    while(true) {
                        auto res = tokenizer_copy.advance();
                        if(res.is_error()) {
                            return false;
                        }
                        auto token = res.unwrap_value();
                        if(token && is_pointer_ref(token.unwrap())) {
                            return true;
                        } else if(
                            token
                            && token.unwrap().type == token_type::identifier
                            && is_microsoft_calling_convention(token.unwrap().str)
                        ) {
                            // pass
                        } else {
                            break;
                        }
                    }
                    return false;
                }
                return false;
            } (tokenizer);
            if(res.is_error()) {
                return false;
            }
            return res.unwrap_value();
        }

        NODISCARD Result<bool, parse_error> parse_function_pointer() {
            ASSERT(lookahead_is_function_pointer());
            TRY_TOK(opening_parenthesis, tokenizer.accept({token_type::punctuation, "("}));
            if(opening_parenthesis) {
                bool saw_pointer = false;
                while(true) {
                    TRY_TOK(next, tokenizer.peek());
                    if(next && is_pointer_ref(next.unwrap())) {
                        tokenizer.advance();
                        saw_pointer = true;
                    } else if(
                        next
                        && next.unwrap().type == token_type::identifier
                        && (
                            is_microsoft_calling_convention(next.unwrap().str)
                            || is_ignored_identifier(next.unwrap().str)
                        )
                    ) {
                        tokenizer.advance();
                    } else {
                        break;
                    }
                }
                if(!saw_pointer) {
                    return parse_error{};
                }
                bool did_parse = false;
                reset_output_flag = true;
                TRY_PARSE(parse_symbol(), did_parse = true);
                if(!did_parse) {
                    return parse_error{};
                }
                TRY_TOK(closing_parenthesis, tokenizer.accept({token_type::punctuation, ")"}));
                if(!closing_parenthesis) {
                    return parse_error{};
                }
                return true;
            }
            return false;
        }

        NODISCARD Result<bool, parse_error> parse_symbol_term() {
            TRY_PARSE(accept_anonymous_namespace(), return true);
            TRY_PARSE(accept_operator(), return true);
            TRY_PARSE(accept_identifier_token(), return true);
            TRY_PARSE(accept_lambda_or_unnamed(), return true);
            return false;
        }

        NODISCARD Result<bool, parse_error> consume_punctuation_and_trailing_modifiers() {
            bool did_consume = false;
            while(true) {
                TRY_TOK(token, tokenizer.peek());
                if(!token) {
                    break;
                }
                TRY_PARSE(accept_balanced_punctuation(), {did_consume = true; continue;});
                TRY_PARSE(accept_ignored_identifier(), {did_consume = true; continue;});
                TRY_PARSE(accept_pointer_ref(false), {did_consume = true; continue;});
                break;
            }
            return did_consume;
        }

        NODISCARD Result<bool, parse_error> parse_symbol() {
            bool made_progress = false;
            while(true) {
                TRY_TOK(token, tokenizer.peek());
                if(!token) {
                    break;
                }
                bool did_match_term = false;
                TRY_PARSE(parse_symbol_term(), did_match_term = true);
                if(lookahead_is_function_pointer()) {
                    TRY_PARSE(parse_function_pointer(), did_match_term = true);
                }
                if(did_match_term) {
                    made_progress = true;
                } else {
                    break;
                }
                TRY_PARSE(accept_pointer_ref(true), made_progress = true);
                TRY_PARSE(consume_punctuation_and_trailing_modifiers(), made_progress = true);
                TRY_TOK(scope_resolution, tokenizer.accept({token_type::punctuation, "::"}));
                if(scope_resolution) {
                    append_output(scope_resolution.unwrap());
                    made_progress = true;
                    // for pointer to members
                    TRY_TOK(star, tokenizer.accept({token_type::punctuation, "*"}));
                    if(star) {
                        append_output(star.unwrap());
                    }
                } else {
                    break;
                }
            }
            return made_progress;
        }

    public:
        symbol_parser(symbol_tokenizer& tokenizer) : tokenizer(tokenizer) {}

        NODISCARD Result<monostate, parse_error> parse() {
            while(true) {
                TRY_TOK(token, tokenizer.peek());
                if(!token) {
                    break;
                }
                reset_output_flag = true;
                bool made_progress = false;
                TRY_PARSE(parse_symbol(), made_progress = true);
                if(!made_progress) {
                    return parse_error{};
                }
            }
            return monostate{};
        }

        NODISCARD std::string name() && {
            return std::move(name_output);
        }
    };

    NODISCARD std::string prune_symbol(string_view symbol) {
        detail::symbol_tokenizer tokenizer(symbol);
        detail::symbol_parser parser(tokenizer);
        auto res = parser.parse();
        if(res.is_error()) {
            // error
            return std::string(symbol);
        }
        auto name = std::move(parser).name();
        if(name.empty()) {
            return std::string(symbol);
        }
        return name;
    }
}
CPPTRACE_END_NAMESPACE

CPPTRACE_BEGIN_NAMESPACE
    std::string prune_symbol(const std::string& symbol) {
        try {
            return detail::prune_symbol(symbol);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return symbol;
        }
    }
CPPTRACE_END_NAMESPACE
