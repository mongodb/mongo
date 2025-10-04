/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/sdkutils/private/endpoints_regex.h>

/*
 * Minimal regex implementation.
 * Inspired by
 * https://www.cs.princeton.edu/courses/archive/spr09/cos333/beautiful.html and
 * https://github.com/kokke/tiny-regex-c.
 *
 * Why write our own regex implementation?
 * Unfortunately, state of cross-platform regex support for c is rather limited.
 * Posix has regex support, but implementation support varies cross platform.
 * Windows supports regex, but only exposes it through C++ interface.
 * For 3p implementations tiny-regex-c comes closest to what we need, but has
 * several deal-breaking limitations, ex. not being thread safe, lack of
 * alternations support.
 * Other 3p C implementations are very bloated for what we need.
 * Hence, since we need a very minimal regex support for endpoint resolution we
 * just implement our own.
 *
 * What is supported?
 * - ascii only matching (no unicode or other encoding support)
 * - multithread safe iterative matching (stack friendly, since this is
 *   typically called deep in call stack)
 * - char matching (plain ascii chars, alpha/digit wildcards)
 * - star and plus (refer to limitations sections for limitations on how they work)
 * - alternation groups
 *
 * Limitations?
 * - star and plus are greedy (match as much as they can), but do not backtrace.
 *   This is major deviation from how regex matching should work.
 *   Note: regions in aws have a predefined pattern where sections are separated
 *   by '-', so current implementation just matches until it hits separator.
 * - grouping using ( and ) is only supported for alternations.
 * - regex must match the whole text, i.e. start with ^ and end with $
 * - features not called out above are not supported
 * - alternations pick first occurrence that matches and do not backtrack to see
 *   if there are any other occurrences
 *
 * Examples
 * current implementation is targeted towards matching typical aws region
 * patterns like "^(us|eu|ap|sa|ca|me|af|il)\\-\\w+\\-\\d+$" (aws partition) or
 * "^us\\-gov\\-\\w+\\-\\d+$" (aws gov partition).
 * All current regions follow
 * "country code(2 chars)-meta(like gov or iso, optional)-direction-digit"
 * and implementation should provide enough features to match those regions.
 * Patterns that would not match correctly are things like "a*a" (star will
 * exhaustively match a and will not give last a back) or (ab|abc), which will
 * not match abc because alternation will lock into ab.
 */

enum regex_symbol_type {
    AWS_ENDPOINTS_REGEX_SYMBOL_DOT,
    AWS_ENDPOINTS_REGEX_SYMBOL_STAR,
    AWS_ENDPOINTS_REGEX_SYMBOL_PLUS,
    AWS_ENDPOINTS_REGEX_SYMBOL_DIGIT,
    AWS_ENDPOINTS_REGEX_SYMBOL_ALPHA,
    AWS_ENDPOINTS_REGEX_SYMBOL_CHAR,
    AWS_ENDPOINTS_REGEX_SYMBOL_ALTERNATION_GROUP,
};

struct aws_endpoints_regex_symbol {
    enum regex_symbol_type type;

    union {
        uint8_t ch;
        struct aws_string *alternation;
    } info;
};

struct aws_endpoints_regex {
    struct aws_array_list symbols;
};

/* Somewhat arbitrary limits on size of regex and text to avoid overly large
 * inputs. */
enum {
    s_max_regex_length = 60,
    s_max_text_length = 50,
    s_max_elements_per_alteration = 20,
};

static void s_clean_up_symbols(struct aws_array_list *symbols) {
    for (size_t i = 0; i < aws_array_list_length(symbols); ++i) {
        struct aws_endpoints_regex_symbol *element = NULL;
        aws_array_list_get_at_ptr(symbols, (void **)&element, i);

        if (element->type == AWS_ENDPOINTS_REGEX_SYMBOL_ALTERNATION_GROUP) {
            aws_string_destroy(element->info.alternation);
        }
    }
}

int s_validate_regex(const struct aws_endpoints_regex *regex) {
    AWS_FATAL_PRECONDITION(regex != NULL);

    for (size_t sym_idx = 0; sym_idx < aws_array_list_length(&regex->symbols); ++sym_idx) {
        struct aws_endpoints_regex_symbol *symbol = NULL;
        aws_array_list_get_at_ptr(&regex->symbols, (void **)&symbol, sym_idx);

        if (symbol->type == AWS_ENDPOINTS_REGEX_SYMBOL_PLUS || symbol->type == AWS_ENDPOINTS_REGEX_SYMBOL_STAR) {

            /* first symbol */
            if (sym_idx == 0) {
                AWS_LOGF_ERROR(
                    AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Invalid regex pattern. Regex cannot start with star or plus.");
                return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            }

            struct aws_endpoints_regex_symbol *prev_symbol = NULL;
            aws_array_list_get_at_ptr(&regex->symbols, (void **)&prev_symbol, sym_idx - 1);

            /* reasonable symbol before */
            enum regex_symbol_type prev_type = prev_symbol->type;
            if (!(prev_type == AWS_ENDPOINTS_REGEX_SYMBOL_DOT || prev_type == AWS_ENDPOINTS_REGEX_SYMBOL_DIGIT ||
                  prev_type == AWS_ENDPOINTS_REGEX_SYMBOL_ALPHA || prev_type == AWS_ENDPOINTS_REGEX_SYMBOL_CHAR ||
                  prev_type == AWS_ENDPOINTS_REGEX_SYMBOL_ALTERNATION_GROUP)) {
                AWS_LOGF_ERROR(
                    AWS_LS_SDKUTILS_ENDPOINTS_REGEX,
                    "Unsupported regex pattern. Star or plus after unsupported character.");
                return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX);
            }

            /* ends with - delimiter */
            if (sym_idx != aws_array_list_length(&regex->symbols) - 1) {
                struct aws_endpoints_regex_symbol *next_symbol = NULL;
                aws_array_list_get_at_ptr(&regex->symbols, (void **)&next_symbol, sym_idx + 1);

                if (next_symbol->type != AWS_ENDPOINTS_REGEX_SYMBOL_CHAR || next_symbol->info.ch != '-') {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_REGEX,
                        "Unsupported regex pattern. Star or plus must be followed by - delimiter.");
                    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX);
                }
            }
        } else if (symbol->type == AWS_ENDPOINTS_REGEX_SYMBOL_ALTERNATION_GROUP) {

            struct aws_byte_cursor alternation = aws_byte_cursor_from_string(symbol->info.alternation);

            /* Not empty */
            if (alternation.len == 0) {
                AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Invalid regex pattern. Empty group.");
                return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            }

            /* Verify that group is only used for alternation. */
            for (size_t i = 0; i < alternation.len; ++i) {
                if (!aws_isalnum(alternation.ptr[i]) && alternation.ptr[i] != '|') {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_REGEX,
                        "Unsupported regex pattern. Only alternation groups are supported.");
                    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX);
                }
            }

            /* alternation elements are unique and not subsets of each other */
            struct aws_byte_cursor elements[s_max_elements_per_alteration];
            size_t num_elements = 0;
            struct aws_byte_cursor split = {0};
            while (aws_byte_cursor_next_split(&alternation, '|', &split)) {
                if (num_elements == s_max_elements_per_alteration) {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Unsupported regex pattern. Too many element in alternation");
                    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX);
                }

                if (split.len == 0) {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Invalid regex pattern. Alternation element cannot be empty");
                    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                }

                elements[num_elements] = split;
                ++num_elements;
            }

            struct aws_byte_cursor input;
            struct aws_byte_cursor prefix;
            for (size_t i = 0; i < num_elements; ++i) {
                for (size_t j = i + 1; j < num_elements; ++j) {

                    if (elements[i].len <= elements[j].len) {
                        input = elements[j];
                        prefix = elements[i];
                    } else {
                        input = elements[i];
                        prefix = elements[j];
                    }

                    if (aws_byte_cursor_starts_with(&input, &prefix)) {
                        AWS_LOGF_ERROR(
                            AWS_LS_SDKUTILS_ENDPOINTS_REGEX,
                            "Unsupported regex pattern. One alternation element cannot be a prefix of another "
                            "element.");
                        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX);
                    }
                }
            }
        }
    }

    return AWS_OP_SUCCESS;
}

struct aws_endpoints_regex *aws_endpoints_regex_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor regex_pattern) {

    if (regex_pattern.len == 0 || regex_pattern.len > s_max_regex_length) {
        AWS_LOGF_ERROR(
            AWS_LS_SDKUTILS_ENDPOINTS_REGEX,
            "Invalid regex pattern size. Must be between 1 and %d",
            s_max_regex_length);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (regex_pattern.ptr[0] != '^' || regex_pattern.ptr[regex_pattern.len - 1] != '$') {
        AWS_LOGF_ERROR(
            AWS_LS_SDKUTILS_ENDPOINTS_REGEX,
            "Unsupported regex pattern. Supported patterns must match the whole text.");
        aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX);
        return NULL;
    }

    /* Ignore begin/end chars */
    aws_byte_cursor_advance(&regex_pattern, 1);
    --regex_pattern.len;

    struct aws_endpoints_regex *re = aws_mem_calloc(allocator, 1, sizeof(struct aws_endpoints_regex));
    aws_array_list_init_dynamic(&re->symbols, allocator, regex_pattern.len, sizeof(struct aws_endpoints_regex_symbol));

    while (regex_pattern.len > 0) {
        uint8_t ch = regex_pattern.ptr[0];
        aws_byte_cursor_advance(&regex_pattern, 1);

        struct aws_endpoints_regex_symbol symbol;
        switch (ch) {
            case '.':
                symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_DOT;
                break;
            case '*':
                symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_STAR;
                break;
            case '+':
                symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_PLUS;
                break;
            case '\\':
                if (regex_pattern.len == 0) {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Invalid regex pattern. Pattern ends with escape character.");
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    goto on_error;
                }
                switch (regex_pattern.ptr[0]) {
                    /* Predefined patterns */
                    case 'd':
                        symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_DIGIT;
                        break;
                    case 'w':
                        symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_ALPHA;
                        break;

                    /* Escaped chars, ex. * or + */
                    default:
                        symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_CHAR;
                        symbol.info.ch = regex_pattern.ptr[0];
                        break;
                }
                aws_byte_cursor_advance(&regex_pattern, 1);
                break;
            case '(': {
                struct aws_byte_cursor group = {0};
                if (!aws_byte_cursor_next_split(&regex_pattern, ')', &group)) {
                    AWS_LOGF_ERROR(AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Invalid regex pattern. Invalid group syntax.");
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    goto on_error;
                }

                aws_byte_cursor_advance(&regex_pattern, group.len);
                if (regex_pattern.len == 0 || regex_pattern.ptr[0] != ')') {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Invalid regex pattern. Missing closing parenthesis.");
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    goto on_error;
                }
                aws_byte_cursor_advance(&regex_pattern, 1);

                symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_ALTERNATION_GROUP;
                symbol.info.alternation = aws_string_new_from_cursor(allocator, &group);
                break;
            }

            default: {
                if (!aws_isalnum(ch)) {
                    AWS_LOGF_ERROR(
                        AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Unsupported regex pattern. Unknown character %c", ch);
                    aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_UNSUPPORTED_REGEX);
                    goto on_error;
                }

                symbol.type = AWS_ENDPOINTS_REGEX_SYMBOL_CHAR;
                symbol.info.ch = ch;
                break;
            }
        }

        aws_array_list_push_back(&re->symbols, &symbol);
    }

    if (s_validate_regex(re)) {
        goto on_error;
    }

    return re;

on_error:
    aws_endpoints_regex_destroy(re);
    return NULL;
}

void aws_endpoints_regex_destroy(struct aws_endpoints_regex *regex) {
    if (regex == NULL) {
        return;
    }

    struct aws_allocator *allocator = regex->symbols.alloc;
    s_clean_up_symbols(&regex->symbols);
    aws_array_list_clean_up(&regex->symbols);
    aws_mem_release(allocator, &regex->symbols);
}

static bool s_match_one(const struct aws_endpoints_regex_symbol *symbol, struct aws_byte_cursor *text) {
    if (text->len == 0) {
        return false;
    }

    uint8_t ch = text->ptr[0];
    switch (symbol->type) {
        case AWS_ENDPOINTS_REGEX_SYMBOL_ALPHA:
            return aws_isalpha(ch);
        case AWS_ENDPOINTS_REGEX_SYMBOL_DIGIT:
            return aws_isdigit(ch);
        case AWS_ENDPOINTS_REGEX_SYMBOL_CHAR:
            return ch == symbol->info.ch;
        case AWS_ENDPOINTS_REGEX_SYMBOL_DOT:
            return true;
        default:
            AWS_FATAL_ASSERT(true);
    }

    return false;
}

static void s_match_star(const struct aws_endpoints_regex_symbol *symbol, struct aws_byte_cursor *text) {
    while (s_match_one(symbol, text)) {
        aws_byte_cursor_advance(text, 1);
    }
}

static bool s_match_plus(const struct aws_endpoints_regex_symbol *symbol, struct aws_byte_cursor *text) {
    if (!s_match_one(symbol, text)) {
        return false;
    }

    aws_byte_cursor_advance(text, 1);
    s_match_star(symbol, text);
    return true;
}

int aws_endpoints_regex_match(const struct aws_endpoints_regex *regex, struct aws_byte_cursor text) {
    AWS_PRECONDITION(regex);

    if (text.len == 0 || text.len > s_max_text_length) {
        AWS_LOGF_ERROR(
            AWS_LS_SDKUTILS_ENDPOINTS_REGEX, "Invalid text size. Must be between 1 and %d", s_max_text_length);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    for (size_t i = 0; i < aws_array_list_length(&regex->symbols); ++i) {
        struct aws_endpoints_regex_symbol *symbol = NULL;
        aws_array_list_get_at_ptr(&regex->symbols, (void **)&symbol, i);

        /* looks forward to check if symbol has * or + modifier */
        if (i + 1 < aws_array_list_length(&regex->symbols)) {
            struct aws_endpoints_regex_symbol *next_symbol = NULL;
            aws_array_list_get_at_ptr(&regex->symbols, (void **)&next_symbol, i + 1);

            if (next_symbol->type == AWS_ENDPOINTS_REGEX_SYMBOL_STAR ||
                next_symbol->type == AWS_ENDPOINTS_REGEX_SYMBOL_PLUS) {
                if (next_symbol->type == AWS_ENDPOINTS_REGEX_SYMBOL_STAR) {
                    s_match_star(symbol, &text);
                } else if (next_symbol->type == AWS_ENDPOINTS_REGEX_SYMBOL_PLUS) {
                    if (!s_match_plus(symbol, &text)) {
                        return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_REGEX_NO_MATCH);
                    }
                }
                ++i;
                continue;
            }
        }

        switch (symbol->type) {
            case AWS_ENDPOINTS_REGEX_SYMBOL_ALTERNATION_GROUP: {
                struct aws_byte_cursor variant = {0};
                struct aws_byte_cursor alternation = aws_byte_cursor_from_string(symbol->info.alternation);
                size_t chars_in_match = 0;
                while (aws_byte_cursor_next_split(&alternation, '|', &variant)) {
                    if (aws_byte_cursor_starts_with(&text, &variant)) {
                        chars_in_match = variant.len;
                        break;
                    }
                }

                if (chars_in_match == 0) {
                    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_REGEX_NO_MATCH);
                }
                aws_byte_cursor_advance(&text, chars_in_match);
                break;
            }
            default:
                if (!s_match_one(symbol, &text)) {
                    return aws_raise_error(AWS_ERROR_SDKUTILS_ENDPOINTS_REGEX_NO_MATCH);
                }
                aws_byte_cursor_advance(&text, 1);
                break;
        }
    }

    return AWS_OP_SUCCESS;
}
