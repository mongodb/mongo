/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"

/*
 * Unit tests for config-related functions.
 */

TEST_CASE("Parse integer", "[config][parse_int]")
{
    SECTION("No conversion")
    {
        const char *s = "abc";
        char *endptr = nullptr;
        int64_t val = __wti_config_parse_dec(s, strlen(s), &endptr);
        REQUIRE(val == 0);
        REQUIRE(endptr == s);
    }

    SECTION("Boundary conditions")
    {
        const char *max_s = "9223372036854775807"; /* INT64_MAX */
        char *endptr_max = nullptr;
        const int64_t max_val = __wti_config_parse_dec(max_s, strlen(max_s), &endptr_max);
        REQUIRE(max_val == INT64_MAX);
        REQUIRE(endptr_max == max_s + strlen(max_s));

        const char *min_s = "-9223372036854775808"; /* INT64_MIN */
        char *endptr_min = nullptr;
        const int64_t min_val = __wti_config_parse_dec(min_s, strlen(min_s), &endptr_min);
        REQUIRE(min_val == INT64_MIN);
        REQUIRE(endptr_min == min_s + strlen(min_s));
    }

    SECTION("Boundary conditions less than one")
    {
        const char *max_s = "9223372036854775806"; /* INT64_MAX - 1 */
        char *endptr_max = nullptr;
        const int64_t max_val = __wti_config_parse_dec(max_s, strlen(max_s), &endptr_max);
        REQUIRE(max_val == INT64_MAX - 1);
        REQUIRE(endptr_max == max_s + strlen(max_s));

        const char *min_s = "-9223372036854775807"; /* INT64_MIN + 1 */
        char *endptr_min = nullptr;
        const int64_t min_val = __wti_config_parse_dec(min_s, strlen(min_s), &endptr_min);
        REQUIRE(min_val == INT64_MIN + 1);
        REQUIRE(endptr_min == min_s + strlen(min_s));
    }

    SECTION("Out of range")
    {
        const char *overflow_s = "9223372036854775808"; /* INT64_MAX + 1 */
        char *endptr_overflow = nullptr;
        errno = 0;
        const int64_t overflow_val =
          __wti_config_parse_dec(overflow_s, strlen(overflow_s), &endptr_overflow);
        REQUIRE(errno == ERANGE);
        REQUIRE(overflow_val == INT64_MAX);
        REQUIRE(endptr_overflow == overflow_s + strlen(overflow_s));

        const char *underflow_s = "-9223372036854775809"; /* INT64_MIN - 1 */
        char *endptr_underflow = nullptr;
        errno = 0;
        const int64_t underflow_val =
          __wti_config_parse_dec(underflow_s, strlen(underflow_s), &endptr_underflow);
        REQUIRE(errno == ERANGE);
        REQUIRE(underflow_val == INT64_MIN);
        REQUIRE(endptr_underflow == underflow_s + strlen(underflow_s));

        const char *longer_s = "123456789012345678901"; /* Longer than INT64_MAX */
        char *endptr_longer = nullptr;
        errno = 0;
        const int64_t longer_val =
          __wti_config_parse_dec(longer_s, strlen(longer_s), &endptr_longer);
        REQUIRE(errno == ERANGE);
        REQUIRE(longer_val == INT64_MAX);
        REQUIRE(endptr_longer == longer_s + strlen(longer_s));
    }

    SECTION("Limited length")
    {
        const char *s = "123";
        char *endptr = nullptr;
        int64_t val = __wti_config_parse_dec(s, 2, &endptr);
        REQUIRE(val == 12);
        REQUIRE(endptr == s + 2);
    }

    SECTION("Stopping at non-digit")
    {
        const char *s = "123abc";
        char *endptr = nullptr;
        int64_t val = __wti_config_parse_dec(s, strlen(s), &endptr);
        REQUIRE(val == 123);
        REQUIRE(endptr == s + 3);

        const char *blank_s = "   789 ";
        char *endptr_blank = nullptr;
        int64_t val_blank = __wti_config_parse_dec(blank_s, strlen(blank_s), &endptr_blank);
        REQUIRE(val_blank == 789);
        REQUIRE(endptr_blank == blank_s + 6);

        const char *pos_s = "   +123 ";
        char *endptr_pos = nullptr;
        int64_t pos_val = __wti_config_parse_dec(pos_s, strlen(pos_s), &endptr_pos);
        REQUIRE(pos_val == 123);
        REQUIRE(endptr_pos == pos_s + 7);

        const char *neg_s = "   -456 ";
        char *endptr_neg = nullptr;
        int64_t neg_val = __wti_config_parse_dec(neg_s, strlen(neg_s), &endptr_neg);
        REQUIRE(neg_val == -456);
        REQUIRE(endptr_neg == neg_s + 7);
    }

    SECTION("Explicit positive and negative")
    {
        const char *pos_s = "+456";
        char *endptr_pos = nullptr;
        int64_t pos_val = __wti_config_parse_dec(pos_s, strlen(pos_s), &endptr_pos);
        REQUIRE(pos_val == 456);
        REQUIRE(endptr_pos == pos_s + strlen(pos_s));

        const char *neg_s = "-789";
        char *endptr_neg = nullptr;
        int64_t neg_val = __wti_config_parse_dec(neg_s, strlen(neg_s), &endptr_neg);
        REQUIRE(neg_val == -789);
        REQUIRE(endptr_neg == neg_s + strlen(neg_s));

        const char *posz_s = "+0";
        char *endptr_posz = nullptr;
        int64_t posz_val = __wti_config_parse_dec(posz_s, strlen(posz_s), &endptr_posz);
        REQUIRE(posz_val == 0);
        REQUIRE(endptr_posz == posz_s + strlen(posz_s));

        const char *negz_s = "-0";
        char *endptr_negz = nullptr;
        int64_t negz_val = __wti_config_parse_dec(negz_s, strlen(negz_s), &endptr_negz);
        REQUIRE(negz_val == 0);
        REQUIRE(endptr_negz == negz_s + strlen(negz_s));
    }
}
