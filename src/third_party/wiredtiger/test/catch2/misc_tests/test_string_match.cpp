/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"

TEST_CASE("Test string matching macro: WT_STRING_MATCH", "[STRING_MATCH]")
{
    WT_CONFIG_ITEM green, empty;
    int i;
    const char *empty_str, *g_str, *greed_str, *green_str, *greener_str;
    const char *null_str, *something_str;

    /* The "right side" of the match */
    green.str = "green";
    green.len = 5;
    empty.str = "";
    empty.len = 0;

    /* The "left side" of the match */
    empty_str = "";
    g_str = "g";
    greed_str = "greed";
    green_str = "green";
    greener_str = "greener";
    null_str = NULL;
    something_str = "something";

    /*
     * Thwart smart compilers: if they know this variable is null, they'll complain about its use
     * with standard string functions, which may be used by the macros we are testing.
     */
    if (getenv("__never__") != NULL)
        null_str = "null";

    SECTION("Check for literal string matching")
    {
        CHECK(null_str == NULL);

        /* We run the test twice, once with the config string null terminated, one without. */
        for (i = 0; i < 2; i++) {
            CHECK(WT_STRING_LIT_MATCH("g", green.str, green.len) == false);
            CHECK(WT_STRING_LIT_MATCH("greed", green.str, green.len) == false);
            CHECK(WT_STRING_LIT_MATCH("green", green.str, green.len) == true);
            CHECK(WT_STRING_LIT_MATCH("greener", green.str, green.len) == false);
            CHECK(WT_STRING_LIT_MATCH("", green.str, green.len) == false);

            CHECK(WT_STRING_LIT_MATCH("", empty.str, empty.len) == true);
            CHECK(WT_STRING_LIT_MATCH("something", empty.str, empty.len) == false);
            CHECK(WT_STRING_LIT_MATCH("", null_str, 0) == true);

            /*
             * The following line will not compile, as it's a misuse of the macro.
             *   CHECK(WT_STRING_LIT_MATCH(green_str, green.str, green.len) == true);
             */

            /* Run all the same tests with the config equivalent macros. */
            CHECK(WT_CONFIG_LIT_MATCH("g", green) == false);
            CHECK(WT_CONFIG_LIT_MATCH("greed", green) == false);
            CHECK(WT_CONFIG_LIT_MATCH("green", green) == true);
            CHECK(WT_CONFIG_LIT_MATCH("greener", green) == false);
            CHECK(WT_CONFIG_LIT_MATCH("", green) == false);

            CHECK(WT_CONFIG_LIT_MATCH("", empty) == true);
            CHECK(WT_CONFIG_LIT_MATCH("something", empty) == false);

            green.str = "greenery";
        }
    }

    SECTION("Check for generic string matching")
    {
        /* We run the test twice, once with the config string null terminated, one without. */
        green.str = "green";
        for (i = 0; i < 2; i++) {
            /*
             * The plain string match macros should still work with literal strings, even though we
             * prefer to use the *_LIT_* macros with literals.
             */
            CHECK(WT_STRING_MATCH("g", green.str, green.len) == false);
            CHECK(WT_STRING_MATCH("greed", green.str, green.len) == false);
            CHECK(WT_STRING_MATCH("green", green.str, green.len) == true);
            CHECK(WT_STRING_MATCH("greener", green.str, green.len) == false);
            CHECK(WT_STRING_MATCH("", green.str, green.len) == false);

            CHECK(WT_STRING_MATCH("", empty.str, empty.len) == true);
            CHECK(WT_STRING_MATCH("something", empty.str, empty.len) == false);
            CHECK(WT_STRING_MATCH("", null_str, 0) == true);

            /*
             * The plain string match macros work with string variables, the *_LIT_* macros will
             * not.
             */
            CHECK(WT_STRING_MATCH(g_str, green.str, green.len) == false);
            CHECK(WT_STRING_MATCH(greed_str, green.str, green.len) == false);
            CHECK(WT_STRING_MATCH(green_str, green.str, green.len) == true);
            CHECK(WT_STRING_MATCH(greener_str, green.str, green.len) == false);
            CHECK(WT_STRING_MATCH(empty_str, green.str, green.len) == false);

            CHECK(WT_STRING_MATCH(empty_str, empty.str, empty.len) == true);
            CHECK(WT_STRING_MATCH(something_str, empty.str, empty.len) == false);
            CHECK(WT_STRING_MATCH(empty_str, null_str, 0) == true);

            /* Run all the same tests with the config equivalent macros. */
            CHECK(WT_CONFIG_MATCH("g", green) == false);
            CHECK(WT_CONFIG_MATCH("greed", green) == false);
            CHECK(WT_CONFIG_MATCH("green", green) == true);
            CHECK(WT_CONFIG_MATCH("greener", green) == false);
            CHECK(WT_CONFIG_MATCH("", green) == false);

            CHECK(WT_CONFIG_MATCH("", empty) == true);
            CHECK(WT_CONFIG_MATCH("something", empty) == false);

            CHECK(WT_CONFIG_MATCH(g_str, green) == false);
            CHECK(WT_CONFIG_MATCH(greed_str, green) == false);
            CHECK(WT_CONFIG_MATCH(green_str, green) == true);
            CHECK(WT_CONFIG_MATCH(greener_str, green) == false);
            CHECK(WT_CONFIG_MATCH(empty_str, green) == false);

            CHECK(WT_CONFIG_MATCH(empty_str, empty) == true);
            CHECK(WT_CONFIG_MATCH(something_str, empty) == false);

            green.str = "greenery";
        }
    }
}
