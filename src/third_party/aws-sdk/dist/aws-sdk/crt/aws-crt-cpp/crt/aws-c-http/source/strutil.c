/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/private/strutil.h>

static struct aws_byte_cursor s_trim(struct aws_byte_cursor cursor, const bool trim_table[256]) {
    /* trim leading whitespace */
    size_t i;
    for (i = 0; i < cursor.len; ++i) {
        const uint8_t c = cursor.ptr[i];
        if (!trim_table[c]) {
            break;
        }
    }
    cursor.ptr += i;
    cursor.len -= i;

    /* trim trailing whitespace */
    for (; cursor.len; --cursor.len) {
        const uint8_t c = cursor.ptr[cursor.len - 1];
        if (!trim_table[c]) {
            break;
        }
    }

    return cursor;
}

static const bool s_http_whitespace_table[256] = {
    [' '] = true,
    ['\t'] = true,
};

struct aws_byte_cursor aws_strutil_trim_http_whitespace(struct aws_byte_cursor cursor) {
    return s_trim(cursor, s_http_whitespace_table);
}

/* RFC7230 section 3.2.6:
 *  token          = 1*tchar
 *  tchar          = "!" / "#" / "$" / "%" / "&" / "'" / "*"
 *                 / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
 *                 / DIGIT / ALPHA
 */
static const bool s_http_token_table[256] = {
    ['!'] = true, ['#'] = true, ['$'] = true, ['%'] = true, ['&'] = true, ['\''] = true, ['*'] = true, ['+'] = true,
    ['-'] = true, ['.'] = true, ['^'] = true, ['_'] = true, ['`'] = true, ['|'] = true,  ['~'] = true,

    ['0'] = true, ['1'] = true, ['2'] = true, ['3'] = true, ['4'] = true, ['5'] = true,  ['6'] = true, ['7'] = true,
    ['8'] = true, ['9'] = true,

    ['A'] = true, ['B'] = true, ['C'] = true, ['D'] = true, ['E'] = true, ['F'] = true,  ['G'] = true, ['H'] = true,
    ['I'] = true, ['J'] = true, ['K'] = true, ['L'] = true, ['M'] = true, ['N'] = true,  ['O'] = true, ['P'] = true,
    ['Q'] = true, ['R'] = true, ['S'] = true, ['T'] = true, ['U'] = true, ['V'] = true,  ['W'] = true, ['X'] = true,
    ['Y'] = true, ['Z'] = true,

    ['a'] = true, ['b'] = true, ['c'] = true, ['d'] = true, ['e'] = true, ['f'] = true,  ['g'] = true, ['h'] = true,
    ['i'] = true, ['j'] = true, ['k'] = true, ['l'] = true, ['m'] = true, ['n'] = true,  ['o'] = true, ['p'] = true,
    ['q'] = true, ['r'] = true, ['s'] = true, ['t'] = true, ['u'] = true, ['v'] = true,  ['w'] = true, ['x'] = true,
    ['y'] = true, ['z'] = true,
};

/* Same as above, but with uppercase characters removed */
static const bool s_http_lowercase_token_table[256] = {
    ['!'] = true, ['#'] = true, ['$'] = true, ['%'] = true, ['&'] = true, ['\''] = true, ['*'] = true, ['+'] = true,
    ['-'] = true, ['.'] = true, ['^'] = true, ['_'] = true, ['`'] = true, ['|'] = true,  ['~'] = true,

    ['0'] = true, ['1'] = true, ['2'] = true, ['3'] = true, ['4'] = true, ['5'] = true,  ['6'] = true, ['7'] = true,
    ['8'] = true, ['9'] = true,

    ['a'] = true, ['b'] = true, ['c'] = true, ['d'] = true, ['e'] = true, ['f'] = true,  ['g'] = true, ['h'] = true,
    ['i'] = true, ['j'] = true, ['k'] = true, ['l'] = true, ['m'] = true, ['n'] = true,  ['o'] = true, ['p'] = true,
    ['q'] = true, ['r'] = true, ['s'] = true, ['t'] = true, ['u'] = true, ['v'] = true,  ['w'] = true, ['x'] = true,
    ['y'] = true, ['z'] = true,
};

static bool s_is_token(struct aws_byte_cursor token, const bool token_table[256]) {
    if (token.len == 0) {
        return false;
    }

    for (size_t i = 0; i < token.len; ++i) {
        const uint8_t c = token.ptr[i];
        if (token_table[c] == false) {
            return false;
        }
    }

    return true;
}

bool aws_strutil_is_http_token(struct aws_byte_cursor token) {
    return s_is_token(token, s_http_token_table);
}

bool aws_strutil_is_lowercase_http_token(struct aws_byte_cursor token) {
    return s_is_token(token, s_http_lowercase_token_table);
}

/* clang-format off */
/**
 * Table with true for all octets allowed in field-content,
 * as defined in RFC7230 section 3.2 and 3.2.6 and RFC5234 appendix-B.1:
 *
 * field-content  = field-vchar [ 1*( SP / HTAB ) field-vchar ]
 * field-vchar    = VCHAR / obs-text
 * VCHAR          = %x21-7E ; visible (printing) characters
 * obs-text       = %x80-FF
 */
static const bool s_http_field_content_table[256] = {
    /* clang-format off */

    /* whitespace */
    ['\t'] = true, [' '] = true,

    /* VCHAR = 0x21-7E */
    [0x21] = true, [0x22] = true, [0x23] = true, [0x24] = true, [0x25] = true, [0x26] = true, [0x27] = true,
    [0x28] = true, [0x29] = true, [0x2A] = true, [0x2B] = true, [0x2C] = true, [0x2D] = true, [0x2E] = true,
    [0x2F] = true, [0x30] = true, [0x31] = true, [0x32] = true, [0x33] = true, [0x34] = true, [0x35] = true,
    [0x36] = true, [0x37] = true, [0x38] = true, [0x39] = true, [0x3A] = true, [0x3B] = true, [0x3C] = true,
    [0x3D] = true, [0x3E] = true, [0x3F] = true, [0x40] = true, [0x41] = true, [0x42] = true, [0x43] = true,
    [0x44] = true, [0x45] = true, [0x46] = true, [0x47] = true, [0x48] = true, [0x49] = true, [0x4A] = true,
    [0x4B] = true, [0x4C] = true, [0x4D] = true, [0x4E] = true, [0x4F] = true, [0x50] = true, [0x51] = true,
    [0x52] = true, [0x53] = true, [0x54] = true, [0x55] = true, [0x56] = true, [0x57] = true, [0x58] = true,
    [0x59] = true, [0x5A] = true, [0x5B] = true, [0x5C] = true, [0x5D] = true, [0x5E] = true, [0x5F] = true,
    [0x60] = true, [0x61] = true, [0x62] = true, [0x63] = true, [0x64] = true, [0x65] = true, [0x66] = true,
    [0x67] = true, [0x68] = true, [0x69] = true, [0x6A] = true, [0x6B] = true, [0x6C] = true, [0x6D] = true,
    [0x6E] = true, [0x6F] = true, [0x70] = true, [0x71] = true, [0x72] = true, [0x73] = true, [0x74] = true,
    [0x75] = true, [0x76] = true, [0x77] = true, [0x78] = true, [0x79] = true, [0x7A] = true, [0x7B] = true,
    [0x7C] = true, [0x7D] = true, [0x7E] = true,

    /* obs-text = %x80-FF */
    [0x80] = true, [0x81] = true, [0x82] = true, [0x83] = true, [0x84] = true, [0x85] = true, [0x86] = true,
    [0x87] = true, [0x88] = true, [0x89] = true, [0x8A] = true, [0x8B] = true, [0x8C] = true, [0x8D] = true,
    [0x8E] = true, [0x8F] = true, [0x90] = true, [0x91] = true, [0x92] = true, [0x93] = true, [0x94] = true,
    [0x95] = true, [0x96] = true, [0x97] = true, [0x98] = true, [0x99] = true, [0x9A] = true, [0x9B] = true,
    [0x9C] = true, [0x9D] = true, [0x9E] = true, [0x9F] = true, [0xA0] = true, [0xA1] = true, [0xA2] = true,
    [0xA3] = true, [0xA4] = true, [0xA5] = true, [0xA6] = true, [0xA7] = true, [0xA8] = true, [0xA9] = true,
    [0xAA] = true, [0xAB] = true, [0xAC] = true, [0xAD] = true, [0xAE] = true, [0xAF] = true, [0xB0] = true,
    [0xB1] = true, [0xB2] = true, [0xB3] = true, [0xB4] = true, [0xB5] = true, [0xB6] = true, [0xB7] = true,
    [0xB8] = true, [0xB9] = true, [0xBA] = true, [0xBB] = true, [0xBC] = true, [0xBD] = true, [0xBE] = true,
    [0xBF] = true, [0xC0] = true, [0xC1] = true, [0xC2] = true, [0xC3] = true, [0xC4] = true, [0xC5] = true,
    [0xC6] = true, [0xC7] = true, [0xC8] = true, [0xC9] = true, [0xCA] = true, [0xCB] = true, [0xCC] = true,
    [0xCD] = true, [0xCE] = true, [0xCF] = true, [0xD0] = true, [0xD1] = true, [0xD2] = true, [0xD3] = true,
    [0xD4] = true, [0xD5] = true, [0xD6] = true, [0xD7] = true, [0xD8] = true, [0xD9] = true, [0xDA] = true,
    [0xDB] = true, [0xDC] = true, [0xDD] = true, [0xDE] = true, [0xDF] = true, [0xE0] = true, [0xE1] = true,
    [0xE2] = true, [0xE3] = true, [0xE4] = true, [0xE5] = true, [0xE6] = true, [0xE7] = true, [0xE8] = true,
    [0xE9] = true, [0xEA] = true, [0xEB] = true, [0xEC] = true, [0xED] = true, [0xEE] = true, [0xEF] = true,
    [0xF0] = true, [0xF1] = true, [0xF2] = true, [0xF3] = true, [0xF4] = true, [0xF5] = true, [0xF6] = true,
    [0xF7] = true, [0xF8] = true, [0xF9] = true, [0xFA] = true, [0xFB] = true, [0xFC] = true, [0xFD] = true,
    [0xFE] = true, [0xFF] = true,
    /* clang-format on */
};

/**
 * From RFC7230 section 3.2:
 * field-value    = *( field-content / obs-fold )
 * field-content  = field-vchar [ 1*( SP / HTAB ) field-vchar ]
 *
 * But we're forbidding obs-fold
 */
bool aws_strutil_is_http_field_value(struct aws_byte_cursor cursor) {
    if (cursor.len == 0) {
        return true;
    }

    /* first and last char cannot be whitespace */
    const uint8_t first_c = cursor.ptr[0];
    const uint8_t last_c = cursor.ptr[cursor.len - 1];
    if (s_http_whitespace_table[first_c] || s_http_whitespace_table[last_c]) {
        return false;
    }

    /* ensure every char is legal field-content */
    size_t i = 0;
    do {
        const uint8_t c = cursor.ptr[i++];
        if (s_http_field_content_table[c] == false) {
            return false;
        }
    } while (i < cursor.len);

    return true;
}

/**
 * From RFC7230 section 3.1.2:
 * reason-phrase  = *( HTAB / SP / VCHAR / obs-text )
 * VCHAR          = %x21-7E ; visible (printing) characters
 * obs-text       = %x80-FF
 */
bool aws_strutil_is_http_reason_phrase(struct aws_byte_cursor cursor) {
    for (size_t i = 0; i < cursor.len; ++i) {
        const uint8_t c = cursor.ptr[i];
        /* the field-content table happens to allow the exact same characters as reason-phrase */
        if (s_http_field_content_table[c] == false) {
            return false;
        }
    }
    return true;
}

bool aws_strutil_is_http_request_target(struct aws_byte_cursor cursor) {
    if (cursor.len == 0) {
        return false;
    }

    /* TODO: Actually check the complete grammar as defined in RFC7230 5.3 and
     * RFC3986. Currently this just checks whether the sequence is blatantly illegal */
    size_t i = 0;
    do {
        const uint8_t c = cursor.ptr[i++];
        /* everything <= ' ' is non-visible ascii*/
        if (c <= ' ') {
            return false;
        }
    } while (i < cursor.len);

    return true;
}

bool aws_strutil_is_http_pseudo_header_name(struct aws_byte_cursor cursor) {
    if (cursor.len == 0) {
        return false;
    }
    const uint8_t c = cursor.ptr[0];
    if (c != ':') {
        /* short cut */
        return false;
    }
    return true;
}
