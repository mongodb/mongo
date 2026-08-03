/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <cstring>

#include "wt_internal.h"
#include "wrappers/mock_session.h"

TEST_CASE("Disagg replace checkpoint", "[disagg][replace_checkpoint]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *s = session_mock->get_wt_session_impl();

    auto make_item = [](const char *str, WT_CONFIG_ITEM::WT_CONFIG_ITEM_TYPE type) {
        WT_CONFIG_ITEM item;
        WT_CLEAR(item);
        item.str = str;
        item.len = strlen(str);
        item.type = type;
        return item;
    };

    SECTION("Substitutes checkpoint and preserves neighbors")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM new_ckpt = make_item("(new=2)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(
                  s, "a=1,checkpoint=(old=1),b=2", &new_ckpt, &replaced) == 0);
        REQUIRE(strcmp(replaced, "a=1,checkpoint=(new=2),b=2") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Replaces a sole checkpoint key")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM new_ckpt = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "checkpoint=(old)", &new_ckpt, &replaced) == 0);
        REQUIRE(strcmp(replaced, "checkpoint=(new)") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Empty replacement value")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("", WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);

        REQUIRE(
          __ut_disagg_replace_checkpoint(s, "a=1,checkpoint=(old),b=2", &val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "a=1,checkpoint=,b=2") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Only the last of duplicate checkpoint keys is substituted")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(
                  s, "checkpoint=(one),a=1,checkpoint=(two)", &val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "checkpoint=(one),a=1,checkpoint=(new)") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Does not match a key that merely shares a prefix")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(
                  s, "checkpoint_abc=(keep),checkpoint=(old),b=2", &val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "checkpoint_abc=(keep),checkpoint=(new),b=2") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Missing checkpoint returns WT_NOTFOUND and does not invent it")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "a=1,b=2", &val, &replaced) == WT_NOTFOUND);
        REQUIRE(replaced == nullptr);
    }

    SECTION("Empty base returns WT_NOTFOUND")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "", &val, &replaced) == WT_NOTFOUND);
        REQUIRE(replaced == nullptr);
    }

    SECTION("Prefix-only key does not satisfy an exact match")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "checkpoint_abc=(keep),b=2", &val, &replaced) ==
          WT_NOTFOUND);
        REQUIRE(replaced == nullptr);
    }

    SECTION("Nested struct values with commas survive rewrite")
    {
        char *replaced = nullptr;
        const char *new_ckpt = "(WiredTigerCheckpoint=(time=1,size=2,addr=\"abc\"))";
        WT_CONFIG_ITEM val = make_item(new_ckpt, WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s,
                  "a=1,checkpoint=(WiredTigerCheckpoint=(time=0,size=0,addr=\"old\")),b=2", &val,
                  &replaced) == 0);
        REQUIRE(strcmp(replaced,
                  "a=1,checkpoint=(WiredTigerCheckpoint=(time=1,size=2,addr=\"abc\")),b=2") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Replacement may shrink or grow the value")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM short_val = make_item("()", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);
        WT_CONFIG_ITEM long_val =
          make_item("(11111111111111111111)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(
                  s, "checkpoint=(quite_a_long_old_value),b=2", &short_val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "checkpoint=(),b=2") == 0);
        __wt_free(s, replaced);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "checkpoint=(),b=2", &long_val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "checkpoint=(11111111111111111111),b=2") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Bare sibling keys must not be rewritten")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "a,checkpoint,b=2", &val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "a=,checkpoint=(new),b=2") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Bare checkpoint key is given the new value")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "a=1,checkpoint,b=2", &val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "a=1,checkpoint=(new),b=2") == 0);
        __wt_free(s, replaced);
    }

    SECTION("Empty checkpoint= value is replaced")
    {
        char *replaced = nullptr;
        WT_CONFIG_ITEM val = make_item("(new)", WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);

        REQUIRE(__ut_disagg_replace_checkpoint(s, "a=1,checkpoint=,b=2", &val, &replaced) == 0);
        REQUIRE(strcmp(replaced, "a=1,checkpoint=(new),b=2") == 0);
        __wt_free(s, replaced);
    }
}
