/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <iostream>
#include <regex>
#include <sstream>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "../wrappers/item_wrapper.h"

static int
insert_key_value(WT_CURSOR *cursor, const char *key, const char *value)
{
    item_wrapper item_key(key);
    item_wrapper item_value(value);
    __wt_cursor_set_raw_key(cursor, item_key.get_item());
    __wt_cursor_set_raw_value(cursor, item_value.get_item());
    return cursor->insert(cursor);
}

static void
insert_key_value(
  WT_CURSOR *cursor1, WT_CURSOR *cursor2, std::string const &key, std::string const &value)
{
    insert_key_value(cursor1, key.c_str(), value.c_str());
    insert_key_value(cursor2, key.c_str(), value.c_str());
}

static void
insert_sample_values(WT_CURSOR *cursor1, WT_CURSOR *cursor2, int first_value, int num_values)
{
    for (int loop = 0; loop <= num_values; loop++) {
        int i = first_value + loop;
        std::stringstream key;
        key << "key" << i;
        std::stringstream value;
        value << "value" << i;
        insert_key_value(cursor1, cursor2, key.str(), value.str());
    }
}

static std::string
parse_blkmods(WT_SESSION *session, std::string const &file_uri)
{
    WT_CURSOR *metadata_cursor = nullptr;
    REQUIRE(session->open_cursor(session, "metadata:", nullptr, nullptr, &metadata_cursor) == 0);

    metadata_cursor->set_key(metadata_cursor, file_uri.c_str());
    REQUIRE(metadata_cursor->search(metadata_cursor) == 0);

    char *file_config;
    REQUIRE(metadata_cursor->get_value(metadata_cursor, &file_config) == 0);

    std::cmatch match_results;
    REQUIRE(std::regex_search(file_config, match_results, std::regex(",blocks=(\\w+)")));
    std::string hex_blkmod = match_results[1];

    REQUIRE(metadata_cursor->close(metadata_cursor) == 0);

    return hex_blkmod;
}

/*
 * convert_hex_string_to_byte_vector converts a hex string into a vector of bytes.
 *
 * If the string contains invalided hex characters it will throw a std::invalid_argument exception.
 */
static std::vector<uint8_t>
convert_hex_string_to_byte_vector(std::string const &source_hex_string)
{
    // Confirm that we have a string with an even number of hex digits
    REQUIRE((source_hex_string.size() % 2) == 0);

    std::vector<uint8_t> byte_vector;

    for (int index = 0; index < (source_hex_string.size() / 2); index++) {
        int const string_index = index * 2;
        std::string const sub_string = source_hex_string.substr(string_index, 2);
        unsigned int value = std::stoi(sub_string, nullptr, 16);
        REQUIRE(0 <= value);
        REQUIRE(value <= 255);

        uint8_t byte_value = value;
        byte_vector.push_back(byte_value);
    }

    return byte_vector;
}

/*
 * is_new_blkmods_ok() Returns true if all bits that were 1 in orig_blkmod_table are still 1 in
 * new_blkmod_table. Otherwise, it returns 0.
 */
static bool
is_new_blkmods_ok(std::string const &orig_blkmod_table, std::string const &new_blkmod_table)
{
    /*
     * If any bits that were 1 in the original blkmod changed, we have an issue.
     */

    std::vector<uint8_t> orig_blkmod_as_bytes(convert_hex_string_to_byte_vector(orig_blkmod_table));
    std::vector<uint8_t> new_blkmod_as_bytes(convert_hex_string_to_byte_vector(new_blkmod_table));

    for (size_t index = 0; index < orig_blkmod_as_bytes.size(); index++) {
        uint8_t orig_blkmod_hex_value = orig_blkmod_as_bytes[index];
        uint8_t new_blkmod_hex_value = 0;

        // If there is a corresponding byte value in the new blkmods then use it, otherwise use 0
        if (index < new_blkmod_as_bytes.size())
            new_blkmod_hex_value = new_blkmod_as_bytes[index];

        uint8_t reverted_bits =
          (orig_blkmod_hex_value ^ new_blkmod_hex_value) & orig_blkmod_hex_value;

        if (reverted_bits != 0)
            return false;
    }

    return true;
}

static bool
test_check_incorrect_modified_bits(
  std::string const &orig_bitmap, std::string const &new_bitmap, int expected_result = 0)
{
    WT_ITEM orig_item, new_item;
    WT_CLEAR(orig_item);
    WT_CLEAR(new_item);

    WT_RET(__wt_buf_init(nullptr, &orig_item, 256));
    WT_RET(__wt_buf_init(nullptr, &new_item, 256));

    REQUIRE(__wt_nhex_to_raw(nullptr, orig_bitmap.c_str(), orig_bitmap.length(), &orig_item) == 0);
    REQUIRE(__wt_nhex_to_raw(nullptr, new_bitmap.c_str(), new_bitmap.length(), &new_item) == 0);

    bool is_ok = false;
    REQUIRE(__ut_ckpt_verify_modified_bits(&orig_item, &new_item, &is_ok) == expected_result);

    __wt_buf_free(nullptr, &orig_item);
    __wt_buf_free(nullptr, &new_item);

    return is_ok;
}

TEST_CASE("Backup: Test convert_hex_string_to_byte_vector()", "[backup]")
{
    std::string hex_source_string = "feffff0700000000";
    std::vector<uint8_t> byte_vector{0xfe, 0xff, 0xff, 0x07, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(convert_hex_string_to_byte_vector(hex_source_string) == byte_vector);

    REQUIRE_THROWS_AS(convert_hex_string_to_byte_vector("invalid hex "), std::invalid_argument);
}

TEST_CASE("Backup: Test is_new_blkmods_ok() - simple", "[backup]")
{
    REQUIRE(is_new_blkmods_ok("10", "10"));
    REQUIRE(is_new_blkmods_ok("10", "30"));

    REQUIRE_FALSE(is_new_blkmods_ok("10", "00"));
    REQUIRE_FALSE(is_new_blkmods_ok("10", "20"));
    REQUIRE_FALSE(is_new_blkmods_ok("10", ""));
}

TEST_CASE("Backup: Test is_new_blkmods_ok()", "[backup]")
{
    std::string orig_blkmod_table1 = "feffff0700000000";
    std::string orig_blkmod_table2 = "feffff0700000000";
    std::string orig_blkmod_table3 = "feffff0700000000";

    // new_blkmod_table1 is ok
    std::string new_blkmod_table1 = "ffffffff01000000";
    // new_blkmod_table2 is not ok, as some bits have switched to 0
    std::string new_blkmod_table2 = "ff0fffff01000000";
    // new_blkmod_table3 is not ok, as it is shorter than the original and some set
    // bits have been lost
    std::string new_blkmod_table3 = "ffffff";

    bool is_table1_ok = is_new_blkmods_ok(orig_blkmod_table1, new_blkmod_table1);
    bool is_table2_ok = is_new_blkmods_ok(orig_blkmod_table2, new_blkmod_table2);
    bool is_table3_ok = is_new_blkmods_ok(orig_blkmod_table3, new_blkmod_table3);

    REQUIRE(is_table1_ok);
    REQUIRE_FALSE(is_table2_ok);
    REQUIRE_FALSE(is_table3_ok);

    REQUIRE(is_new_blkmods_ok("10", "10"));
}

TEST_CASE("Backup: null pointer params to __check_incorrect_modified_bits()", "[backup]")
{
    bool is_ok;
    WT_ITEM item;

    is_ok = false;
    WT_CLEAR(item);

    REQUIRE(__ut_ckpt_verify_modified_bits(&item, nullptr, &is_ok) == EINVAL);
    REQUIRE(__ut_ckpt_verify_modified_bits(nullptr, &item, &is_ok) == EINVAL);
    REQUIRE(__ut_ckpt_verify_modified_bits(nullptr, nullptr, &is_ok) == EINVAL);
}

TEST_CASE("Backup: check modified bits - simple", "[backup]")
{
    REQUIRE(test_check_incorrect_modified_bits("10", "10"));
    REQUIRE(test_check_incorrect_modified_bits("10", "30"));
    REQUIRE(test_check_incorrect_modified_bits("60", "70"));
    REQUIRE(test_check_incorrect_modified_bits("e0", "f0"));

    REQUIRE_FALSE(test_check_incorrect_modified_bits("10", "00"));
    REQUIRE_FALSE(test_check_incorrect_modified_bits("10", "20"));
    REQUIRE_FALSE(test_check_incorrect_modified_bits("10", "", EINVAL));
}

TEST_CASE("Backup: check modified bits", "[backup]")
{
    SECTION("Simpler cases")
    {
        std::string orig_blkmod_table1 = "feffff0700000000";
        std::string orig_blkmod_table2 = "feffff0700000000";
        std::string orig_blkmod_table3 = "feffff0700000000";

        // new_blkmod_table1 is ok
        std::string new_blkmod_table1 = "ffffffff01000000";
        // new_blkmod_table2 is not ok, as some bits have switched to 0
        std::string new_blkmod_table2 = "ff0fffff01000000";
        // new_blkmod_table3 is not ok, as it is shorter than the original and some set
        // bits have been lost
        std::string new_blkmod_table3 = "ffffff";

        bool is_table1_ok =
          test_check_incorrect_modified_bits(orig_blkmod_table1, new_blkmod_table1);
        bool is_table2_ok =
          test_check_incorrect_modified_bits(orig_blkmod_table2, new_blkmod_table2);
        bool is_table3_ok =
          test_check_incorrect_modified_bits(orig_blkmod_table3, new_blkmod_table3, EINVAL);

        REQUIRE(is_table1_ok);
        REQUIRE_FALSE(is_table2_ok);
        REQUIRE_FALSE(is_table3_ok);
    }

    SECTION("Longer patterns")
    {
        // These blk_mod patterns match those from "Backup: Test blkmods in incremental backup"

        std::string orig_blkmod_table1 = "feffff07000000000000000000000000";

        // new_blkmod_table1 is ok
        std::string new_blkmod_table1 = "ffffffff010000000000000000000000";

        bool is_table1_ok =
          test_check_incorrect_modified_bits(orig_blkmod_table1, new_blkmod_table1);

        REQUIRE(is_table1_ok);

        bool is_table1_ok2 = is_new_blkmods_ok(orig_blkmod_table1, new_blkmod_table1);

        REQUIRE(is_table1_ok2);
    }
}

TEST_CASE("Backup: Test blkmods in incremental backup", "[backup]")
{
    std::string create_config = "allocation_size=512,key_format=S,value_format=S";
    std::string backup_config = "incremental=(enabled,granularity=4k,this_id=\"ID1\")";

    std::string uri1 = "backup_test1";
    std::string uri2 = "backup_test2";
    std::string file1_uri = "file:" + uri1 + ".wt";
    std::string file2_uri = "file:" + uri2 + ".wt";
    std::string table1_uri = "table:" + uri1;
    std::string table2_uri = "table:" + uri2;

    const int num_few_keys = 100;
    const int num_more_keys = 5000;

    std::string orig_blkmod_table1;
    std::string orig_blkmod_table2;
    std::string new_blkmod_table1;
    std::string new_blkmod_table2;

    {
        // Setup
        std::string conn_config =
          "create,file_manager=(close_handle_minimum=0,close_idle_time=3,close_scan_interval=1),"
          "statistics=(fast)";
        connection_wrapper conn(DB_HOME, conn_config.c_str());
        conn.clear_do_cleanup();
        WT_SESSION_IMPL *session_impl = conn.create_session();

        WT_SESSION *session = &session_impl->iface;

        REQUIRE(session->create(session, table1_uri.c_str(), create_config.c_str()) == 0);
        REQUIRE(session->create(session, table2_uri.c_str(), create_config.c_str()) == 0);

        WT_CURSOR *cursor1 = nullptr;
        REQUIRE(session->open_cursor(session, table1_uri.c_str(), nullptr, nullptr, &cursor1) == 0);

        WT_CURSOR *cursor2 = nullptr;
        REQUIRE(session->open_cursor(session, table2_uri.c_str(), nullptr, nullptr, &cursor2) == 0);

        insert_sample_values(cursor1, cursor2, 0, num_few_keys);
        REQUIRE(session->checkpoint(session, nullptr) == 0);

        WT_CURSOR *backup_cursor = nullptr;
        REQUIRE(session->open_cursor(
                  session, "backup:", nullptr, backup_config.c_str(), &backup_cursor) == 0);
        REQUIRE(backup_cursor->close(backup_cursor) == 0);

        insert_sample_values(cursor1, cursor2, num_few_keys, num_more_keys);
        REQUIRE(session->checkpoint(session, nullptr) == 0);

        REQUIRE(cursor1->close(cursor1) == 0);
        REQUIRE(cursor2->close(cursor2) == 0);

        REQUIRE(session->checkpoint(session, nullptr) == 0);

        orig_blkmod_table1 = parse_blkmods(session, file1_uri);
        orig_blkmod_table2 = parse_blkmods(session, file2_uri);
    }

    {
        // Incremental backup and validate
        std::string conn_config =
          "file_manager=(close_handle_minimum=0,close_idle_time=3,close_scan_interval=1),"
          "statistics=(fast)";
        connection_wrapper conn(DB_HOME, conn_config.c_str());
        WT_SESSION_IMPL *session_impl = conn.create_session();

        WT_SESSION *session = &session_impl->iface;

        REQUIRE(session->create(session, table1_uri.c_str(), create_config.c_str()) == 0);
        REQUIRE(session->create(session, table2_uri.c_str(), create_config.c_str()) == 0);

        WT_CURSOR *cursor1 = nullptr;
        REQUIRE(session->open_cursor(session, table1_uri.c_str(), nullptr, nullptr, &cursor1) == 0);

        WT_CURSOR *cursor2 = nullptr;
        REQUIRE(session->open_cursor(session, table2_uri.c_str(), nullptr, nullptr, &cursor2) == 0);

        REQUIRE(insert_key_value(cursor1, "key5000", "value5000") == 0);
        REQUIRE(session->checkpoint(session, nullptr) == 0);

        REQUIRE(insert_key_value(cursor2, "key5000", "value5000") == 0);
        REQUIRE(session->checkpoint(session, nullptr) == 0);

        REQUIRE(insert_key_value(cursor1, "key5000", "value5000") == 0);
        REQUIRE(session->checkpoint(session, nullptr) == 0);

        new_blkmod_table1 = parse_blkmods(session, file1_uri);

        REQUIRE(insert_key_value(cursor2, "key5000", "value5000") == 0);
        REQUIRE(session->checkpoint(session, nullptr) == 0);

        new_blkmod_table2 = parse_blkmods(session, file2_uri);

        REQUIRE(cursor1->close(cursor1) == 0);
        REQUIRE(cursor2->close(cursor2) == 0);
    }

    bool is_table1_ok = is_new_blkmods_ok(orig_blkmod_table1, new_blkmod_table1);
    bool is_table2_ok = is_new_blkmods_ok(orig_blkmod_table2, new_blkmod_table2);

    REQUIRE(is_table1_ok);
    REQUIRE(is_table2_ok);
}
