/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Cross-check __wt_modify_result_may_be_in_tombstone_namespace against actually applying the modify
 * vector: the prediction must never miss a result that is in the namespace. The prediction is exact
 * unless an entry disturbs the marker positions without covering them with its data, in which case
 * it may over-report; vectors are checked accordingly.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"

namespace {

struct mod_entry {
    std::string data;
    size_t offset;
    size_t size;
};

/* A connection with mock cursors: the code under test needs only a session and a value format. */
struct prediction_fixture {
    connection_wrapper conn;
    WT_SESSION_IMPL *session;
    WT_CURSOR cur_u;
    WT_CURSOR cur_s;

    prediction_fixture(const std::string &home) : conn(home)
    {
        session = conn.create_session();
        WT_CLEAR(cur_u);
        cur_u.session = (WT_SESSION *)session;
        cur_u.value_format = "u";
        WT_CLEAR(cur_s);
        cur_s.session = (WT_SESSION *)session;
        cur_s.value_format = "S";
    }

    ~prediction_fixture()
    {
        __wt_buf_free(session, &cur_u.value);
        __wt_buf_free(session, &cur_s.value);
    }
};

/*
 * The prediction is exact unless an entry starts at or below the marker positions without its data
 * covering them through.
 */
bool
prediction_is_exact(const std::vector<mod_entry> &entries)
{
    for (const auto &e : entries)
        if (e.offset + e.data.size() <= 1)
            return false;
    return true;
}

/* Predict via the function under test, apply via the cursor API, and compare. */
void
check_case(WT_SESSION_IMPL *session, WT_CURSOR *cursor, const std::string &base,
  const std::vector<mod_entry> &case_entries)
{
    std::vector<WT_MODIFY> entries(case_entries.size());
    for (size_t i = 0; i < case_entries.size(); ++i) {
        entries[i].data.data = case_entries[i].data.data();
        entries[i].data.size = case_entries[i].data.size();
        entries[i].offset = case_entries[i].offset;
        entries[i].size = case_entries[i].size;
    }

    WT_ITEM base_item;
    WT_CLEAR(base_item);
    base_item.data = base.data();
    base_item.size = base.size();

    bool predicted_ns = __wt_modify_result_may_be_in_tombstone_namespace(
      session, cursor->value_format, &base_item, entries.data(), (int)entries.size());

    cursor->value.data = base.data();
    cursor->value.size = base.size();
    REQUIRE(__wt_modify_apply_api(cursor, entries.data(), (int)entries.size()) == 0);

    const uint8_t *result = static_cast<const uint8_t *>(cursor->value.data);
    bool actual_ns =
      cursor->value.size >= 2 && result[0] == (uint8_t)0x14 && result[1] == (uint8_t)0x14;

    if (prediction_is_exact(case_entries))
        CHECK(predicted_ns == actual_ns);
    else
        /* Conservative vectors may over-report the namespace, never miss it. */
        CHECK((predicted_ns || !actual_ns));
}

} // namespace

TEST_CASE(
  "Modify tombstone namespace prediction: directed cases", "[modify][modify_tombstone_namespace]")
{
    const std::string home = "WT_TEST.modify_tombstone_namespace_directed";
    utils::wiredtiger_cleanup(home);

    {
        prediction_fixture fix(home);

        const std::string ts = "\x14\x14";
        const std::string deep(17, 'x');

        std::vector<std::pair<std::string, std::vector<mod_entry>>> cases = {
          /* Rewrite the leading bytes into the namespace. */
          {"abcdef", {{ts, 0, 2}}},
          /* Pad past the end of the value; leading bytes untouched. */
          {"ab", {{"\x14", 5, 3}}},
          /* Append onto an empty value, result exactly the two tombstone bytes. */
          {"", {{ts, 0, 0}}},
          /* Replace through (and past) the end. */
          {ts + "abc", {{"", 1, 100}}},
          /* Delete shifts trailing bytes into the leading positions. */
          {std::string("zz\x14\x14", 4), {{"", 0, 2}}},
          /* Delete shifts non-marker bytes in: an over-report is allowed, a miss is not. */
          {"abcd", {{"", 0, 2}}},
          /* Result shorter than the namespace prefix. */
          {ts, {{"", 0, 1}}},
          /* Grow then shrink: the intermediate exceeds both base and result sizes. */
          {std::string("\x14\x14rest", 6), {{std::string(100, 'x'), 0, 0}, {"", 0, 100}}},
          /* Cumulative offsets: a leading insert shifts what the second entry sees. */
          {"abcd", {{"\x14", 0, 0}, {"\x14", 1, 1}}},
          /* Empty data, zero size: a no-op entry. */
          {"abcd", {{"", 2, 0}}},
          /* Plant the marker deep with one entry, shift it to the front with the next. */
          {deep, {{ts, 15, 2}, {"", 0, 15}}},
          /* Same shape, but the shift leaves the marker short of the front. */
          {deep, {{ts, 15, 2}, {"", 0, 14}}},
          /* An unknown head is cured by a later covering entry. */
          {"abcd", {{"", 0, 2}, {ts, 0, 2}}},
          /* A known marker head is destroyed by a later shrink. */
          {"abcd", {{ts, 0, 2}, {"", 0, 2}}},
          /* One base byte plus one appended byte assemble the marker. */
          {"\x14", {{"\x14", 1, 0}}},
          /* Pad plus data assemble a 2-byte value; the pad byte proves it clean. */
          {"", {{"\x14", 1, 0}}},
          /* Pure pads, no data. */
          {"", {{"", 3, 0}}},
          /* Same-size overwrite of a 1-byte value: too short regardless of content. */
          {"\x14", {{"\x14", 0, 1}}},
          /* Marker base kept in place, the second marker byte written by the entry. */
          {std::string("\x14y", 2), {{"\x14", 1, 1}}},
          /* Tail delete leaves exactly the tombstone bytes; the head is untouched. */
          {ts + "abc", {{"", 2, 3}}},
          /* Shrink-by-one pulls the marker to the front. */
          {"a" + ts, {{"", 0, 1}}},
          /* Shrink-by-one pulls non-marker bytes to the front. */
          {std::string("ab\x14", 3), {{"", 0, 1}}},
        };
        for (auto &c : cases) {
            CAPTURE(c.first, c.second.size());
            check_case(fix.session, &fix.cur_u, c.first, c.second);
        }

        /* String-format cases: the content excludes the trailing nul and the pad byte is ' '. */
        std::vector<std::pair<std::string, std::vector<mod_entry>>> s_cases = {
          /* Rewrite the leading content bytes into the namespace. */
          {std::string("ab\0", 3), {{ts, 0, 2}}},
          /* Append at the content end, past the nul, assembling the marker. */
          {std::string("\x14\0", 2), {{"\x14", 1, 0}}},
          /* Pads below an appending entry's offset are spaces, not marker bytes. */
          {std::string("\0", 1), {{"\x14", 2, 0}}},
          /* Tail delete of the content leaves exactly the tombstone bytes. */
          {ts + std::string("ab\0", 3), {{"", 2, 2}}},
        };
        for (auto &c : s_cases) {
            CAPTURE(c.first, c.second.size());
            check_case(fix.session, &fix.cur_s, c.first, c.second);
        }
    }

    utils::wiredtiger_cleanup(home);
}

TEST_CASE("Modify tombstone namespace prediction: randomized raw format",
  "[modify][modify_tombstone_namespace]")
{
    const std::string home = "WT_TEST.modify_tombstone_namespace_rand_u";
    utils::wiredtiger_cleanup(home);

    {
        prediction_fixture fix(home);

        std::random_device rd;
        const unsigned seed = rd();
        CAPTURE(seed);
        std::mt19937 rng(seed);

        auto rand_byte = [&](void) -> char {
            /* Bias toward the tombstone byte so namespace results are common. */
            return (rng() % 5 == 0) ? '\x14' : (char)(rng() % 256);
        };
        /* Bias toward the marker positions and the value length, where classification changes. */
        auto rand_offset = [&](size_t base_len) -> size_t {
            switch (rng() % 3) {
            case 0:
                return rng() % 3;
            case 1:
                return base_len + 2 - std::min<size_t>(base_len + 2, rng() % 5);
            default:
                return rng() % 56;
            }
        };
        auto rand_len = [&](size_t bound) -> size_t {
            return (rng() % 3 == 0) ? rng() % 3 : rng() % bound;
        };

        std::string base;
        std::vector<mod_entry> entries;
        base.reserve(64);

        for (int trial = 0; trial < 5000; ++trial) {
            CAPTURE(trial);
            base.clear();
            for (size_t i = rng() % 48; i > 0; --i)
                base.push_back(rand_byte());

            entries.assign(1 + rng() % 6, mod_entry());
            for (auto &e : entries) {
                for (size_t i = rand_len(24); i > 0; --i)
                    e.data.push_back(rand_byte());
                e.offset = rand_offset(base.size());
                e.size = rand_len(24);
            }
            check_case(fix.session, &fix.cur_u, base, entries);
        }
    }

    utils::wiredtiger_cleanup(home);
}

TEST_CASE("Modify tombstone namespace prediction: randomized string format",
  "[modify][modify_tombstone_namespace]")
{
    const std::string home = "WT_TEST.modify_tombstone_namespace_rand_s";
    utils::wiredtiger_cleanup(home);

    {
        prediction_fixture fix(home);

        std::random_device rd;
        const unsigned seed = rd();
        CAPTURE(seed);
        std::mt19937 rng(seed);

        auto rand_char = [&](void) -> char {
            return (rng() % 5 == 0) ? '\x14' : (char)(1 + rng() % 255);
        };
        auto rand_offset = [&](size_t content_len) -> size_t {
            switch (rng() % 3) {
            case 0:
                return rng() % 3;
            case 1:
                return content_len + 2 - std::min<size_t>(content_len + 2, rng() % 5);
            default:
                return rng() % 40;
            }
        };
        auto rand_len = [&](size_t bound) -> size_t {
            return (rng() % 3 == 0) ? rng() % 3 : rng() % bound;
        };

        std::string base;
        std::vector<mod_entry> entries;
        base.reserve(48);

        for (int trial = 0; trial < 2000; ++trial) {
            CAPTURE(trial);
            base.clear();
            for (size_t i = rng() % 32; i > 0; --i)
                base.push_back(rand_char());
            base.push_back('\0');

            entries.assign(1 + rng() % 4, mod_entry());
            for (auto &e : entries) {
                for (size_t i = rand_len(16); i > 0; --i)
                    e.data.push_back(rand_char());
                e.offset = rand_offset(base.size() - 1);
                e.size = rand_len(16);
            }
            check_case(fix.session, &fix.cur_s, base, entries);
        }
    }

    utils::wiredtiger_cleanup(home);
}
