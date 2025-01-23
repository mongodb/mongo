/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/host_utils.h>
#include <aws/common/string.h>
#include <inttypes.h>

#ifdef _MSC_VER /* Disable sscanf warnings on windows. */
#    pragma warning(disable : 4204)
#    pragma warning(disable : 4706)
#    pragma warning(disable : 4996)
#endif

/* 4 octets of 3 chars max + 3 separators + null terminator */
#define AWS_IPV4_STR_LEN 16
#define IP_CHAR_FMT "%03" SCNu16

static bool s_is_ipv6_char(uint8_t value) {
    return aws_isxdigit(value) || value == ':';
}

static bool s_ends_with(struct aws_byte_cursor cur, uint8_t ch) {
    return cur.len > 0 && cur.ptr[cur.len - 1] == ch;
}

bool aws_host_utils_is_ipv4(struct aws_byte_cursor host) {
    if (host.len > AWS_IPV4_STR_LEN - 1) {
        return false;
    }

    char copy[AWS_IPV4_STR_LEN] = {0};
    memcpy(copy, host.ptr, host.len);

    uint16_t octet[4] = {0};
    char remainder[2] = {0};
    if (4 != sscanf(
                 copy,
                 IP_CHAR_FMT "." IP_CHAR_FMT "." IP_CHAR_FMT "." IP_CHAR_FMT "%1s",
                 &octet[0],
                 &octet[1],
                 &octet[2],
                 &octet[3],
                 remainder)) {
        return false;
    }

    for (size_t i = 0; i < 4; ++i) {
        if (octet[i] > 255) {
            return false;
        }
    }

    return true;
}

/* actual encoding is %25, but % is omitted for simplicity, since split removes it */
static struct aws_byte_cursor s_percent_uri_enc = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("25");
/*
 * IPv6 format:
 * 8 groups of 4 hex chars separated by colons (:)
 * leading 0s in each group can be skipped
 * 2 or more consecutive zero groups can be replaced by double colon (::),
 *     but only once.
 * ipv6 literal can be scoped by to zone by appending % followed by zone name
 * ( does not look like there is length reqs on zone name length. this
 * implementation enforces that its > 1 )
 * ipv6 can be embedded in url, in which case % must be uri encoded as %25.
 * Implementation is fairly trivial and just iterates through the string
 * keeping track of the spec above.
 */
bool aws_host_utils_is_ipv6(struct aws_byte_cursor host, bool is_uri_encoded) {
    if (host.len == 0) {
        return false;
    }

    struct aws_byte_cursor substr = {0};
    /* first split is required ipv6 part */
    bool is_split = aws_byte_cursor_next_split(&host, '%', &substr);
    AWS_ASSERT(is_split); /* function is guaranteed to return at least one split */

    if (!is_split || substr.len == 0 || s_ends_with(substr, ':') ||
        !aws_byte_cursor_satisfies_pred(&substr, s_is_ipv6_char)) {
        return false;
    }

    uint8_t group_count = 0;
    bool has_double_colon = false;
    struct aws_byte_cursor group = {0};
    while (aws_byte_cursor_next_split(&substr, ':', &group)) {
        ++group_count;

        if (group_count > 8 ||                                         /* too many groups */
            group.len > 4 ||                                           /* too many chars in group */
            (has_double_colon && group.len == 0 && group_count > 2)) { /* only one double colon allowed */
            return false;
        }

        has_double_colon = has_double_colon || group.len == 0;
    }

    /* second split is optional zone part */
    if (aws_byte_cursor_next_split(&host, '%', &substr)) {
        if ((is_uri_encoded &&
             (substr.len < 3 ||
              !aws_byte_cursor_starts_with(&substr, &s_percent_uri_enc))) || /* encoding for % + 1 extra char */
            (!is_uri_encoded && substr.len == 0) ||                          /* at least 1 char */
            !aws_byte_cursor_satisfies_pred(&substr, aws_isalnum)) {
            return false;
        }
    }

    return has_double_colon ? group_count < 7 : group_count == 8;
}
