/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/uri.h>

#include <aws/common/common.h>

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4221) /* aggregate initializer using local variable addresses */
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

enum parser_state {
    ON_SCHEME,
    ON_AUTHORITY,
    ON_PATH,
    ON_QUERY_STRING,
    FINISHED,
    ERROR,
};

struct uri_parser {
    struct aws_uri *uri;
    enum parser_state state;
};

/* strlen of UINT32_MAX "4294967295" is 10, plus 1 for '\0' */
#define PORT_BUFFER_SIZE 11

typedef void(parse_fn)(struct uri_parser *parser, struct aws_byte_cursor *str);

static void s_parse_scheme(struct uri_parser *parser, struct aws_byte_cursor *str);
static void s_parse_authority(struct uri_parser *parser, struct aws_byte_cursor *str);
static void s_parse_path(struct uri_parser *parser, struct aws_byte_cursor *str);
static void s_parse_query_string(struct uri_parser *parser, struct aws_byte_cursor *str);

static parse_fn *s_states[] = {
    [ON_SCHEME] = s_parse_scheme,
    [ON_AUTHORITY] = s_parse_authority,
    [ON_PATH] = s_parse_path,
    [ON_QUERY_STRING] = s_parse_query_string,
};

static int s_init_from_uri_str(struct aws_uri *uri) {
    struct uri_parser parser = {
        .state = ON_SCHEME,
        .uri = uri,
    };

    struct aws_byte_cursor uri_cur = aws_byte_cursor_from_buf(&uri->uri_str);

    while (parser.state < FINISHED) {
        s_states[parser.state](&parser, &uri_cur);
    }

    /* Each state function sets the next state, if something goes wrong it sets it to ERROR which is > FINISHED */
    if (parser.state == FINISHED) {
        return AWS_OP_SUCCESS;
    }

    aws_byte_buf_clean_up(&uri->uri_str);
    AWS_ZERO_STRUCT(*uri);
    return AWS_OP_ERR;
}

int aws_uri_init_parse(struct aws_uri *uri, struct aws_allocator *allocator, const struct aws_byte_cursor *uri_str) {
    AWS_ZERO_STRUCT(*uri);
    uri->self_size = sizeof(struct aws_uri);
    uri->allocator = allocator;

    if (aws_byte_buf_init_copy_from_cursor(&uri->uri_str, allocator, *uri_str)) {
        return AWS_OP_ERR;
    }

    return s_init_from_uri_str(uri);
}

int aws_uri_init_from_builder_options(
    struct aws_uri *uri,
    struct aws_allocator *allocator,
    struct aws_uri_builder_options *options) {

    AWS_ZERO_STRUCT(*uri);

    if (options->query_string.len && options->query_params) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    uri->self_size = sizeof(struct aws_uri);
    uri->allocator = allocator;

    size_t buffer_size = 0;
    if (options->scheme.len) {
        /* 3 for :// */
        buffer_size += options->scheme.len + 3;
    }

    buffer_size += options->host_name.len;

    if (options->port) {
        buffer_size += PORT_BUFFER_SIZE;
    }

    buffer_size += options->path.len;

    if (options->query_params) {
        size_t query_len = aws_array_list_length(options->query_params);
        if (query_len) {
            /* for the '?' */
            buffer_size += 1;
            for (size_t i = 0; i < query_len; ++i) {
                struct aws_uri_param *uri_param_ptr = NULL;
                int result = aws_array_list_get_at_ptr(options->query_params, (void **)&uri_param_ptr, i);
                AWS_FATAL_ASSERT(result == AWS_OP_SUCCESS);
                /* 2 == 1 for '&' and 1 for '='. who cares if we over-allocate a little?  */
                buffer_size += uri_param_ptr->key.len + uri_param_ptr->value.len + 2;
            }
        }
    } else if (options->query_string.len) {
        /* for the '?' */
        buffer_size += 1;
        buffer_size += options->query_string.len;
    }

    if (aws_byte_buf_init(&uri->uri_str, allocator, buffer_size)) {
        return AWS_OP_ERR;
    }

    uri->uri_str.len = 0;
    if (options->scheme.len) {
        aws_byte_buf_append(&uri->uri_str, &options->scheme);
        struct aws_byte_cursor scheme_app = aws_byte_cursor_from_c_str("://");
        aws_byte_buf_append(&uri->uri_str, &scheme_app);
    }

    aws_byte_buf_append(&uri->uri_str, &options->host_name);

    struct aws_byte_cursor port_app = aws_byte_cursor_from_c_str(":");
    if (options->port) {
        aws_byte_buf_append(&uri->uri_str, &port_app);
        char port_arr[PORT_BUFFER_SIZE] = {0};
        snprintf(port_arr, sizeof(port_arr), "%" PRIu32, options->port);
        struct aws_byte_cursor port_csr = aws_byte_cursor_from_c_str(port_arr);
        aws_byte_buf_append(&uri->uri_str, &port_csr);
    }

    aws_byte_buf_append(&uri->uri_str, &options->path);

    struct aws_byte_cursor query_app = aws_byte_cursor_from_c_str("?");

    if (options->query_params) {
        struct aws_byte_cursor query_param_app = aws_byte_cursor_from_c_str("&");
        struct aws_byte_cursor key_value_delim = aws_byte_cursor_from_c_str("=");

        aws_byte_buf_append(&uri->uri_str, &query_app);
        size_t query_len = aws_array_list_length(options->query_params);
        for (size_t i = 0; i < query_len; ++i) {
            struct aws_uri_param *uri_param_ptr = NULL;
            aws_array_list_get_at_ptr(options->query_params, (void **)&uri_param_ptr, i);
            aws_byte_buf_append(&uri->uri_str, &uri_param_ptr->key);
            aws_byte_buf_append(&uri->uri_str, &key_value_delim);
            aws_byte_buf_append(&uri->uri_str, &uri_param_ptr->value);

            if (i < query_len - 1) {
                aws_byte_buf_append(&uri->uri_str, &query_param_app);
            }
        }
    } else if (options->query_string.len) {
        aws_byte_buf_append(&uri->uri_str, &query_app);
        aws_byte_buf_append(&uri->uri_str, &options->query_string);
    }

    return s_init_from_uri_str(uri);
}

void aws_uri_clean_up(struct aws_uri *uri) {
    if (uri->uri_str.allocator) {
        aws_byte_buf_clean_up(&uri->uri_str);
    }
    AWS_ZERO_STRUCT(*uri);
}

const struct aws_byte_cursor *aws_uri_scheme(const struct aws_uri *uri) {
    return &uri->scheme;
}

const struct aws_byte_cursor *aws_uri_authority(const struct aws_uri *uri) {
    return &uri->authority;
}

const struct aws_byte_cursor *aws_uri_path(const struct aws_uri *uri) {
    return &uri->path;
}

const struct aws_byte_cursor *aws_uri_query_string(const struct aws_uri *uri) {
    return &uri->query_string;
}

const struct aws_byte_cursor *aws_uri_path_and_query(const struct aws_uri *uri) {
    return &uri->path_and_query;
}

const struct aws_byte_cursor *aws_uri_host_name(const struct aws_uri *uri) {
    return &uri->host_name;
}

uint32_t aws_uri_port(const struct aws_uri *uri) {
    return uri->port;
}

bool aws_query_string_next_param(struct aws_byte_cursor query_string, struct aws_uri_param *param) {
    /* If param is zeroed, then this is the first run. */
    bool first_run = param->value.ptr == NULL;

    /* aws_byte_cursor_next_split() is used to iterate over params in the query string.
     * It takes an in/out substring arg similar to how this function works */
    struct aws_byte_cursor substr;
    if (first_run) {
        /* substring must be zeroed to start */
        AWS_ZERO_STRUCT(substr);
    } else {
        /* re-assemble substring which contained key and value */
        substr.ptr = param->key.ptr;
        substr.len = (param->value.ptr - param->key.ptr) + param->value.len;
    }

    /* The do-while is to skip over any empty substrings */
    do {
        if (!aws_byte_cursor_next_split(&query_string, '&', &substr)) {
            /* no more splits, done iterating */
            return false;
        }
    } while (substr.len == 0);

    uint8_t *delim = memchr(substr.ptr, '=', substr.len);
    if (delim) {
        param->key.ptr = substr.ptr;
        param->key.len = delim - substr.ptr;
        param->value.ptr = delim + 1;
        param->value.len = substr.len - param->key.len - 1;
    } else {
        /* no '=', key gets substring, value is blank */
        param->key = substr;
        param->value.ptr = substr.ptr + substr.len;
        param->value.len = 0;
    }

    return true;
}

int aws_query_string_params(struct aws_byte_cursor query_string_cursor, struct aws_array_list *out_params) {
    struct aws_uri_param param;
    AWS_ZERO_STRUCT(param);
    while (aws_query_string_next_param(query_string_cursor, &param)) {
        if (aws_array_list_push_back(out_params, &param)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

bool aws_uri_query_string_next_param(const struct aws_uri *uri, struct aws_uri_param *param) {
    return aws_query_string_next_param(uri->query_string, param);
}

int aws_uri_query_string_params(const struct aws_uri *uri, struct aws_array_list *out_params) {
    return aws_query_string_params(uri->query_string, out_params);
}

static void s_parse_scheme(struct uri_parser *parser, struct aws_byte_cursor *str) {
    const uint8_t *location_of_colon = memchr(str->ptr, ':', str->len);

    if (!location_of_colon) {
        parser->state = ON_AUTHORITY;
        return;
    }

    /* make sure we didn't just pick up the port by mistake */
    if ((size_t)(location_of_colon - str->ptr) < str->len && *(location_of_colon + 1) != '/') {
        parser->state = ON_AUTHORITY;
        return;
    }

    const size_t scheme_len = location_of_colon - str->ptr;
    parser->uri->scheme = aws_byte_cursor_advance(str, scheme_len);

    if (str->len < 3 || str->ptr[0] != ':' || str->ptr[1] != '/' || str->ptr[2] != '/') {
        aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
        parser->state = ERROR;
        return;
    }

    /* advance past the "://" */
    aws_byte_cursor_advance(str, 3);
    parser->state = ON_AUTHORITY;
}

static void s_parse_authority(struct uri_parser *parser, struct aws_byte_cursor *str) {
    const uint8_t *location_of_slash = memchr(str->ptr, '/', str->len);
    const uint8_t *location_of_qmark = memchr(str->ptr, '?', str->len);

    if (!location_of_slash && !location_of_qmark && str->len) {
        parser->uri->authority.ptr = str->ptr;
        parser->uri->authority.len = str->len;

        parser->uri->path.ptr = NULL;
        parser->uri->path.len = 0;
        parser->uri->path_and_query = parser->uri->path;
        parser->state = FINISHED;
        aws_byte_cursor_advance(str, parser->uri->authority.len);
    } else if (!str->len) {
        parser->state = ERROR;
        aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
        return;
    } else {
        const uint8_t *end = str->ptr + str->len;
        if (location_of_slash) {
            parser->state = ON_PATH;
            end = location_of_slash;
        } else if (location_of_qmark) {
            parser->state = ON_QUERY_STRING;
            end = location_of_qmark;
        }

        parser->uri->authority = aws_byte_cursor_advance(str, end - str->ptr);
    }

    struct aws_byte_cursor authority_parse_csr = parser->uri->authority;

    if (authority_parse_csr.len) {
        /* RFC-3986 section 3.2: authority = [ userinfo "@" ] host [ ":" port ] */
        const uint8_t *userinfo_delim = memchr(authority_parse_csr.ptr, '@', authority_parse_csr.len);
        if (userinfo_delim) {

            parser->uri->userinfo =
                aws_byte_cursor_advance(&authority_parse_csr, userinfo_delim - authority_parse_csr.ptr);
            /* For the "@" mark */
            aws_byte_cursor_advance(&authority_parse_csr, 1);
            struct aws_byte_cursor userinfo_parse_csr = parser->uri->userinfo;
            uint8_t *info_delim = memchr(userinfo_parse_csr.ptr, ':', userinfo_parse_csr.len);
            /* RFC-3986 section 3.2.1: Use of the format "user:password" in the userinfo field is deprecated. But we
             * treat the userinfo as URL here, also, if the format is not following URL pattern, you have the whole
             * userinfo */
            /* RFC-1738 section 3.1: <user>:<password> */
            if (info_delim) {
                parser->uri->user.ptr = userinfo_parse_csr.ptr;
                parser->uri->user.len = info_delim - userinfo_parse_csr.ptr;
                parser->uri->password.ptr = info_delim + 1;
                parser->uri->password.len = parser->uri->userinfo.len - parser->uri->user.len - 1;
            } else {
                parser->uri->user = userinfo_parse_csr;
            }
        }

        /* RFC-3986 section 3.2: host identified by IPv6 literal address is
         * enclosed within square brackets. We must ignore any colons within
         * IPv6 literals and only search for port delimiter after closing bracket.*/
        const uint8_t *port_search_start = authority_parse_csr.ptr;
        size_t port_search_len = authority_parse_csr.len;
        bool is_IPv6_literal = false;
        if (authority_parse_csr.len > 0 && authority_parse_csr.ptr[0] == '[') {
            is_IPv6_literal = true;
            port_search_start = memchr(authority_parse_csr.ptr, ']', authority_parse_csr.len);
            if (!port_search_start) {
                parser->state = ERROR;
                aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
                return;
            }
            port_search_len = authority_parse_csr.len - (port_search_start - authority_parse_csr.ptr);
        }

        const uint8_t *port_delim = memchr(port_search_start, ':', port_search_len);
        /*
         * RFC-3986 section 3.2.2: A host identified by an IPv6 literal address is represented inside square
         * brackets.
         * Ignore the square brackets.
         */
        parser->uri->host_name = authority_parse_csr;
        if (is_IPv6_literal) {
            aws_byte_cursor_advance(&parser->uri->host_name, 1);
            parser->uri->host_name.len--;
        }
        if (!port_delim) {
            parser->uri->port = 0;
            return;
        }

        size_t host_name_length_correction = is_IPv6_literal ? 2 : 0;
        parser->uri->host_name.len = port_delim - authority_parse_csr.ptr - host_name_length_correction;
        size_t port_len = authority_parse_csr.len - parser->uri->host_name.len - 1 - host_name_length_correction;
        port_delim += 1;

        uint64_t port_u64 = 0;
        if (port_len > 0) {
            struct aws_byte_cursor port_cursor = aws_byte_cursor_from_array(port_delim, port_len);
            if (aws_byte_cursor_utf8_parse_u64(port_cursor, &port_u64)) {
                parser->state = ERROR;
                aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
                return;
            }
            if (port_u64 > UINT32_MAX) {
                parser->state = ERROR;
                aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
                return;
            }
        }

        parser->uri->port = (uint32_t)port_u64;
    }
}

static void s_parse_path(struct uri_parser *parser, struct aws_byte_cursor *str) {
    parser->uri->path_and_query = *str;

    const uint8_t *location_of_q_mark = memchr(str->ptr, '?', str->len);

    if (!location_of_q_mark) {
        parser->uri->path.ptr = str->ptr;
        parser->uri->path.len = str->len;
        parser->state = FINISHED;
        aws_byte_cursor_advance(str, parser->uri->path.len);
        return;
    }

    if (!str->len) {
        parser->state = ERROR;
        aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
        return;
    }

    parser->uri->path.ptr = str->ptr;
    parser->uri->path.len = location_of_q_mark - str->ptr;
    aws_byte_cursor_advance(str, parser->uri->path.len);
    parser->state = ON_QUERY_STRING;
}

static void s_parse_query_string(struct uri_parser *parser, struct aws_byte_cursor *str) {
    if (!parser->uri->path_and_query.ptr) {
        parser->uri->path_and_query = *str;
    }
    /* we don't want the '?' character. */
    if (str->len) {
        parser->uri->query_string.ptr = str->ptr + 1;
        parser->uri->query_string.len = str->len - 1;
    }

    aws_byte_cursor_advance(str, parser->uri->query_string.len + 1);
    parser->state = FINISHED;
}

static uint8_t s_to_uppercase_hex(uint8_t value) {
    AWS_ASSERT(value < 16);

    if (value < 10) {
        return (uint8_t)('0' + value);
    }

    return (uint8_t)('A' + value - 10);
}

typedef void(unchecked_append_canonicalized_character_fn)(struct aws_byte_buf *buffer, uint8_t value);

/*
 * Appends a character or its hex encoding to the buffer.  We reserve enough space up front so that
 * we can do this with raw pointers rather than multiple function calls/cursors/etc...
 *
 * This function is for the uri path
 */
static void s_unchecked_append_canonicalized_path_character(struct aws_byte_buf *buffer, uint8_t value) {
    AWS_ASSERT(buffer->len + 3 <= buffer->capacity);

    uint8_t *dest_ptr = buffer->buffer + buffer->len;

    if (aws_isalnum(value)) {
        ++buffer->len;
        *dest_ptr = value;
        return;
    }

    switch (value) {
        /* non-alpha-numeric unreserved, don't % encode them */
        case '-':
        case '_':
        case '.':
        case '~':

        /* reserved characters that we should not % encode in the path component */
        case '/':
            ++buffer->len;
            *dest_ptr = value;
            return;

        /*
         * everything else we should % encode, including from the reserved list
         */
        default:
            buffer->len += 3;
            *dest_ptr++ = '%';
            *dest_ptr++ = s_to_uppercase_hex(value >> 4);
            *dest_ptr = s_to_uppercase_hex(value & 0x0F);
            return;
    }
}

/*
 * Appends a character or its hex encoding to the buffer.  We reserve enough space up front so that
 * we can do this with raw pointers rather than multiple function calls/cursors/etc...
 *
 * This function is for query params
 */
static void s_raw_append_canonicalized_param_character(struct aws_byte_buf *buffer, uint8_t value) {
    AWS_ASSERT(buffer->len + 3 <= buffer->capacity);

    uint8_t *dest_ptr = buffer->buffer + buffer->len;

    if (aws_isalnum(value)) {
        ++buffer->len;
        *dest_ptr = value;
        return;
    }

    switch (value) {
        case '-':
        case '_':
        case '.':
        case '~': {
            ++buffer->len;
            *dest_ptr = value;
            return;
        }

        default:
            buffer->len += 3;
            *dest_ptr++ = '%';
            *dest_ptr++ = s_to_uppercase_hex(value >> 4);
            *dest_ptr = s_to_uppercase_hex(value & 0x0F);
            return;
    }
}

/*
 * Writes a cursor to a buffer using the supplied encoding function.
 */
static int s_encode_cursor_to_buffer(
    struct aws_byte_buf *buffer,
    const struct aws_byte_cursor *cursor,
    unchecked_append_canonicalized_character_fn *append_canonicalized_character) {
    const uint8_t *current_ptr = cursor->ptr;
    const uint8_t *end_ptr = cursor->ptr + cursor->len;

    /*
     * reserve room up front for the worst possible case: everything gets % encoded
     */
    size_t capacity_needed = 0;
    if (AWS_UNLIKELY(aws_mul_size_checked(3, cursor->len, &capacity_needed))) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_reserve_relative(buffer, capacity_needed)) {
        return AWS_OP_ERR;
    }

    while (current_ptr < end_ptr) {
        append_canonicalized_character(buffer, *current_ptr);
        ++current_ptr;
    }

    return AWS_OP_SUCCESS;
}

int aws_byte_buf_append_encoding_uri_path(struct aws_byte_buf *buffer, const struct aws_byte_cursor *cursor) {
    return s_encode_cursor_to_buffer(buffer, cursor, s_unchecked_append_canonicalized_path_character);
}

int aws_byte_buf_append_encoding_uri_param(struct aws_byte_buf *buffer, const struct aws_byte_cursor *cursor) {
    return s_encode_cursor_to_buffer(buffer, cursor, s_raw_append_canonicalized_param_character);
}

int aws_byte_buf_append_decoding_uri(struct aws_byte_buf *buffer, const struct aws_byte_cursor *cursor) {
    /* reserve room up front for worst possible case: no % and everything copies over 1:1 */
    if (aws_byte_buf_reserve_relative(buffer, cursor->len)) {
        return AWS_OP_ERR;
    }

    /* advance over cursor */
    struct aws_byte_cursor advancing = *cursor;
    uint8_t c;
    while (aws_byte_cursor_read_u8(&advancing, &c)) {

        if (c == '%') {
            /* two hex characters following '%' are the byte's value */
            if (AWS_UNLIKELY(aws_byte_cursor_read_hex_u8(&advancing, &c) == false)) {
                return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
            }
        }

        buffer->buffer[buffer->len++] = c;
    }

    return AWS_OP_SUCCESS;
}
