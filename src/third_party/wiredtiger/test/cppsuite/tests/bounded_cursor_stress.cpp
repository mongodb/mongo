/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "src/bound/bound_set.h"
#include "src/common/constants.h"
#include "src/common/random_generator.h"
#include "src/bound/bound.h"
#include "src/main/test.h"

using namespace test_harness;

/*
 * In this test, we want to verify the usage of the cursor bound API and check that the cursor
 * returns the correct key when bounds are set.
 * During the test duration:
 *  - M threads will keep inserting new random keys.
 *  - N threads will execute search_near calls with random bounds set. Each search_near
 * call with bounds set is verified using the standard cursor's search and next/prev calls.
 *  - O threads will continuously remove random keys.
 *  - P threads will continuously update random keys.
 *  - Q threads will utilize the custom operation and will execute next() and prev() calls with
 * random bounds set. Both next() and prev() calls with bounds set is verified against the
 * default cursor next() and prev() calls.
 */
class bounded_cursor_stress : public test {
    /* Class helper to represent the lower and uppers bounds for the range cursor. */
    private:
    bool _reverse_collator_enabled = false;
    const uint64_t MAX_ROLLBACKS = 100;
    enum class bound_action { NO_BOUNDS, LOWER_BOUND_SET, UPPER_BOUND_SET, ALL_BOUNDS_SET };

    public:
    bounded_cursor_stress(const test_args &args) : test(args)
    {
        /* Track reverse_collator value as it is required for the custom comparator. */
        _reverse_collator_enabled = _config->get_bool(REVERSE_COLLATOR);
        init_operation_tracker();
    }

    bool
    custom_lexicographical_compare(
      const std::string &first_key, const std::string &second_key, bool inclusive)
    {
        if (_reverse_collator_enabled)
            if (!inclusive)
                return first_key.compare(second_key) > 0;
            else
                return first_key.compare(second_key) >= 0;
        else if (!inclusive)
            return first_key.compare(second_key) < 0;
        else
            return first_key.compare(second_key) <= 0;
    }

    /*
     * Helper function which traverses the tree, given the range cursor and normal cursor. The next
     * variable decides where we traverse forwards or backwards in the tree. Also perform lower
     * bound and upper bound checks while walking the tree.
     */
    void
    cursor_traversal(scoped_cursor &bounded_cursor, scoped_cursor &normal_cursor,
      const bound &lower_bound, const bound &upper_bound, bool next)
    {
        int exact, normal_ret, range_ret;
        exact = normal_ret = range_ret = 0;

        auto lower_key = lower_bound.get_key();
        auto upper_key = upper_bound.get_key();
        if (next) {
            range_ret = bounded_cursor->next(bounded_cursor.get());
            /*
             * If the key exists, position the cursor to the lower key using search near otherwise
             * use prev().
             */
            if (!lower_key.empty()) {
                normal_cursor->set_key(normal_cursor.get(), lower_key.c_str());
                normal_ret = normal_cursor->search_near(normal_cursor.get(), &exact);
                if (normal_ret == WT_NOTFOUND)
                    return;
                if (exact < 0)
                    normal_ret = normal_cursor->next(normal_cursor.get());
            } else
                normal_ret = normal_cursor->next(normal_cursor.get());
        } else {
            range_ret = bounded_cursor->prev(bounded_cursor.get());
            /*
             * If the key exists, position the cursor to the upper key using search near otherwise
             * use next().
             */
            if (!upper_key.empty()) {
                normal_cursor->set_key(normal_cursor.get(), upper_key.c_str());
                normal_ret = normal_cursor->search_near(normal_cursor.get(), &exact);
                if (normal_ret == WT_NOTFOUND)
                    return;
                if (exact > 0)
                    normal_ret = normal_cursor->prev(normal_cursor.get());
            } else
                normal_ret = normal_cursor->prev(normal_cursor.get());
        }

        if (normal_ret == WT_NOTFOUND || normal_ret == WT_ROLLBACK || range_ret == WT_ROLLBACK)
            return;

        const char *normal_key;
        testutil_check(normal_cursor->get_key(normal_cursor.get(), &normal_key));
        /*
         * It is possible that there are no keys within the range. Therefore make sure that normal
         * cursor returns a key that is outside of the range.
         */
        if (range_ret == WT_NOTFOUND) {
            if (next) {
                testutil_assert(!upper_key.empty());
                testutil_assert(!custom_lexicographical_compare(normal_key, upper_key, true));
            } else {
                testutil_assert(!lower_key.empty());
                testutil_assert(custom_lexicographical_compare(normal_key, lower_key, false));
            }
            return;
        }

        testutil_assert(range_ret == 0 && normal_ret == 0);

        /* Retrieve the key the cursor is pointing at. */
        const char *range_key;
        testutil_check(normal_cursor->get_key(normal_cursor.get(), &normal_key));
        testutil_check(bounded_cursor->get_key(bounded_cursor.get(), &range_key));
        testutil_assert(std::string(normal_key).compare(range_key) == 0);
        while (true) {
            if (next) {
                normal_ret = normal_cursor->next(normal_cursor.get());
                range_ret = bounded_cursor->next(bounded_cursor.get());
            } else {
                normal_ret = normal_cursor->prev(normal_cursor.get());
                range_ret = bounded_cursor->prev(bounded_cursor.get());
            }

            if (normal_ret == WT_ROLLBACK || range_ret == WT_ROLLBACK)
                break;

            testutil_assert(normal_ret == 0 || normal_ret == WT_NOTFOUND);
            testutil_assert(range_ret == 0 || range_ret == WT_NOTFOUND);

            /* Early exit if we have reached the end of the table. */
            if (range_ret == WT_NOTFOUND && normal_ret == WT_NOTFOUND)
                break;
            /* It is possible that we have reached the end of the bounded range. */
            else if (range_ret == WT_NOTFOUND && normal_ret == 0) {
                testutil_check(normal_cursor->get_key(normal_cursor.get(), &normal_key));
                /*  Make sure that normal cursor returns a key that is outside of the range. */
                if (next) {
                    testutil_assert(!upper_key.empty());
                    testutil_assert(!custom_lexicographical_compare(normal_key, upper_key, true));
                } else {
                    testutil_assert(!lower_key.empty());
                    testutil_assert(custom_lexicographical_compare(normal_key, lower_key, false));
                }
                break;
            }
            /* Make sure that records match between both cursors. */
            testutil_check(normal_cursor->get_key(normal_cursor.get(), &normal_key));
            testutil_check(bounded_cursor->get_key(bounded_cursor.get(), &range_key));
            testutil_assert(std::string(normal_key).compare(range_key) == 0);
            if (next && !upper_key.empty())
                testutil_assert(custom_lexicographical_compare(
                  range_key, upper_key, upper_bound.get_inclusive()));
            else if (!next && !lower_key.empty())
                testutil_assert(custom_lexicographical_compare(
                  lower_key, range_key, lower_bound.get_inclusive()));
        }
    }

    /*
     * Use the random generator either set no bounds, only lower bounds, only upper bounds or both
     * bounds on the range cursor. The lower and upper bounds are randomly generated strings and the
     * inclusive configuration is also randomly set as well.
     */
    bound_set
    set_random_bounds(thread_worker *tc, scoped_cursor &bounded_cursor)
    {
        bound lower_bound, upper_bound;

        testutil_check(bounded_cursor->reset(bounded_cursor.get()));
        bound_action action =
          static_cast<bound_action>(random_generator::instance().generate_integer(0, 3));
        if (action == bound_action::NO_BOUNDS)
            testutil_check(bounded_cursor->bound(bounded_cursor.get(), "action=clear"));

        if (action == bound_action::LOWER_BOUND_SET || action == bound_action::ALL_BOUNDS_SET)
            lower_bound = bound(tc->key_size, true);

        if (action == bound_action::UPPER_BOUND_SET || action == bound_action::ALL_BOUNDS_SET) {
            if (action == bound_action::ALL_BOUNDS_SET) {
                /* Ensure that the lower and upper bounds are never overlapping. */
                auto diff = _reverse_collator_enabled ? -1 : 1;
                upper_bound = bound(tc->key_size, false, lower_bound.get_key()[0] + diff);
            } else
                upper_bound = bound(tc->key_size, false);
        }

        if (action == bound_action::ALL_BOUNDS_SET)
            testutil_assert(
              custom_lexicographical_compare(lower_bound.get_key(), upper_bound.get_key(), false));

        if (!lower_bound.get_key().empty())
            lower_bound.apply(bounded_cursor);
        if (!upper_bound.get_key().empty())
            upper_bound.apply(bounded_cursor);

        return bound_set(lower_bound, upper_bound);
    }

    /*
     * Validate the bound search call. If the key is within range, the range cursor should return
     * back the search key.
     *
     * Prior to this function call, it's asserted that bounded cursor returns with either a valid
     * key or with WT_NOTFOUND.
     */
    void
    validate_bound_search(int range_ret, scoped_cursor &bounded_cursor,
      const std::string &search_key, const bound_set &bounds)
    {
        auto lower_bound = bounds.get_lower();
        auto upper_bound = bounds.get_upper();
        auto lower_key = lower_bound.get_key();
        auto upper_key = upper_bound.get_key();
        auto lower_inclusive = lower_bound.get_inclusive();
        auto upper_inclusive = upper_bound.get_inclusive();

        const char *key;
        testutil_check(bounded_cursor->get_key(bounded_cursor.get(), &key));
        logger::log_msg(LOG_TRACE,
          "bounded search found key: " + std::string(key) + " with lower bound: " + lower_key +
            " upper bound: " + upper_key);

        /*
         * Assert that if bounded cursor returned with a key, the search key has to be within range.
         */
        auto above_lower_key = lower_key.empty() ||
          custom_lexicographical_compare(lower_key, search_key, lower_inclusive);
        auto below_upper_key = upper_key.empty() ||
          custom_lexicographical_compare(search_key, upper_key, upper_inclusive);

        /*
         * If bounded cursor returns a valid key, search key must have been in bounds. If normal
         * cursor returns a valid key, but bounded cursor returns WT_NOTFOUND, the search key must
         * have been out of bounds.
         */
        if (range_ret == 0) {
            testutil_assert(above_lower_key && below_upper_key);
            /* Check that the returned key should be equal to the search key. */
            testutil_assert(search_key.compare(key) == 0);
        } else
            testutil_assert(!(above_lower_key && below_upper_key));
    }

    /*
     * Validate the bound search_near call. There are three scenarios that needs to be validated
     * differently.
     *  Scenario 1: Range cursor has returned WT_NOTFOUND, this indicates that no records exist in
     * the bounded range. Validate this through traversing all records within the range on a normal
     * cursor.
     *  Scenario 2: Range cursor has returned a key and the search key is outside the range bounds.
     * Validate that the returned key is either the first or last record in the bounds.
     *  Scenario 3: Range cursor has returned a key and the search key is inside the range bounds.
     * Validate that the returned key is visible and that it is indeed the closest key that range
     * cursor could find.
     *
     * Prior to this function call, it's asserted that bounded cursor returns with either a
     * valid key or with WT_NOTFOUND.
     */
    void
    validate_bound_search_near(int range_ret, int range_exact, scoped_cursor &bounded_cursor,
      scoped_cursor &normal_cursor, const std::string &search_key, const bound_set &bounds)
    {
        auto lower_bound = bounds.get_lower();
        auto upper_bound = bounds.get_upper();
        /* Range cursor has successfully returned with a key. */
        if (range_ret == 0) {
            auto lower_key = lower_bound.get_key();
            auto upper_key = upper_bound.get_key();
            auto lower_inclusive = lower_bound.get_inclusive();
            auto upper_inclusive = upper_bound.get_inclusive();

            const char *key;
            testutil_check(bounded_cursor->get_key(bounded_cursor.get(), &key));
            logger::log_msg(LOG_TRACE,
              "bounded search_near found key: " + std::string(key) +
                " with lower bound: " + lower_key + " upper bound: " + upper_key);

            /* Assert that the range cursor has returned a key inside the bounded range. */
            auto above_lower_key =
              lower_key.empty() || custom_lexicographical_compare(lower_key, key, lower_inclusive);
            auto below_upper_key =
              upper_key.empty() || custom_lexicographical_compare(key, upper_key, upper_inclusive);
            testutil_assert(above_lower_key && below_upper_key);

            /* Decide whether the search key is inside or outside the bounded range. */
            above_lower_key = lower_key.empty() ||
              custom_lexicographical_compare(lower_key, search_key, lower_inclusive);
            below_upper_key = upper_key.empty() ||
              custom_lexicographical_compare(search_key, upper_key, upper_inclusive);
            auto search_key_inside_range = above_lower_key && below_upper_key;

            normal_cursor->set_key(normal_cursor.get(), key);
            /* Position the normal cursor on the found key from range cursor. */
            testutil_check(normal_cursor->search(normal_cursor.get()));

            /*
             * Call different validation methods depending on whether the search key is inside or
             * outside the range.
             */
            if (search_key_inside_range)
                validate_successful_search_near_inside_range(
                  normal_cursor, range_exact, search_key);
            else {
                testutil_assert(range_exact != 0);
                validate_successful_search_near_outside_range(
                  normal_cursor, lower_bound, upper_bound, above_lower_key);
            }
            /* Range cursor has not found anything within the set bounds. */
        } else
            validate_search_near_not_found(normal_cursor, lower_bound, upper_bound);
    }

    /*
     * Validate that if the search key is inside the bounded range, that the range cursor has
     * returned a record that is visible and is a viable record that is closest to the search key.
     * We can use exact to perform this validation.
     */
    void
    validate_successful_search_near_inside_range(
      scoped_cursor &normal_cursor, int range_exact, const std::string &search_key)
    {
        int ret = 0;
        /* Retrieve the key the normal cursor is pointing at. */
        const char *key;
        testutil_check(normal_cursor->get_key(normal_cursor.get(), &key));
        logger::log_msg(LOG_TRACE,
          "bounded search_near validating correct returned key with search key inside range as: " +
            search_key + " and exact: " + std::to_string(range_exact));
        /* When exact = 0, the returned key should be equal to the search key. */
        if (range_exact == 0) {
            testutil_assert(search_key.compare(key) == 0);
        } else if (range_exact > 0) {
            /*
             * When exact > 0, the returned key should be greater than the search key and performing
             * a prev() should be less than the search key.
             */
            testutil_assert(!custom_lexicographical_compare(key, search_key, true));

            /* Check that the previous key is less than the search key. */
            ret = normal_cursor->prev(normal_cursor.get());
            testutil_assert(ret == WT_NOTFOUND || ret == 0);
            if (ret == WT_NOTFOUND)
                return;
            testutil_check(normal_cursor->get_key(normal_cursor.get(), &key));
            testutil_assert(custom_lexicographical_compare(key, search_key, false));
            /*
             * When exact < 0, the returned key should be less than the search key and performing a
             * next() should be greater than the search key.
             */
        } else {
            testutil_assert(custom_lexicographical_compare(key, search_key, false));

            /* Check that the next key is greater than the search key. */
            ret = normal_cursor->next(normal_cursor.get());
            testutil_assert(ret == WT_NOTFOUND || ret == 0);
            if (ret == WT_NOTFOUND)
                return;
            testutil_check(normal_cursor->get_key(normal_cursor.get(), &key));
            testutil_assert(!custom_lexicographical_compare(key, search_key, true));
        }
    }

    /*
     * Validate that if the search key is outside the bounded range, that the range cursor has
     * returned a record that is either the first or last entry within the range. Do this through
     * checking if the position of the search key is greater than the range or smaller than the
     * range. Further perform a next or prev call on the normal cursor and we expect that the key is
     * outside of the range.
     */
    void
    validate_successful_search_near_outside_range(scoped_cursor &normal_cursor,
      const bound &lower_bound, const bound &upper_bound, bool larger_search_key)
    {
        int ret = larger_search_key ? normal_cursor->next(normal_cursor.get()) :
                                      normal_cursor->prev(normal_cursor.get());
        auto lower_key = lower_bound.get_key();
        auto upper_key = upper_bound.get_key();
        if (ret == WT_NOTFOUND)
            return;
        testutil_assert(ret == 0);

        const char *key;
        testutil_check(normal_cursor->get_key(normal_cursor.get(), &key));
        /*
         * Assert that the next() or prev() call has placed the normal cursor outside of the bounded
         * range.
         */
        auto above_lower_key = lower_key.empty() ||
          custom_lexicographical_compare(lower_key, key, lower_bound.get_inclusive());
        auto below_upper_key = upper_key.empty() ||
          custom_lexicographical_compare(key, upper_key, upper_bound.get_inclusive());
        testutil_assert(!(above_lower_key && below_upper_key));
    }

    /*
     * Validate that the normal cursor is positioned at a key that is outside of the bounded range,
     * and that no visible keys exist in the bounded range.
     */
    void
    validate_search_near_not_found(
      scoped_cursor &normal_cursor, const bound &lower_bound, const bound &upper_bound)
    {
        int ret = 0, exact = 0;

        auto lower_key = lower_bound.get_key();
        auto upper_key = upper_bound.get_key();
        logger::log_msg(LOG_TRACE,
          "bounded search_near found WT_NOTFOUND on lower bound: " + lower_key + " upper bound: " +
            upper_key + " traversing range to validate that there are no keys within range.");
        if (!lower_key.empty()) {
            normal_cursor->set_key(normal_cursor.get(), lower_key.c_str());
            ret = normal_cursor->search_near(normal_cursor.get(), &exact);
        } else
            ret = normal_cursor->next(normal_cursor.get());

        testutil_assert(ret == 0 || ret == WT_NOTFOUND || ret == WT_ROLLBACK);
        if (ret == WT_ROLLBACK)
            return;
        /*
         * If search near has positioned the cursor before the lower key, perform a next() to to
         * place the cursor in the first record in the range.
         */
        if (exact < 0)
            ret = normal_cursor->next(normal_cursor.get());

        /*
         * Validate that there are no keys in the bounded range that the range cursor could have
         * returned.
         */
        const char *key;
        while (ret != WT_NOTFOUND) {
            testutil_assert(ret == 0);

            testutil_check(normal_cursor->get_key(normal_cursor.get(), &key));
            /* Asserted that the traversed key is not within the range bound. */
            auto above_lower_key = lower_key.empty() ||
              custom_lexicographical_compare(lower_key, key, lower_bound.get_inclusive());
            auto below_upper_key = upper_key.empty() ||
              custom_lexicographical_compare(key, upper_key, upper_bound.get_inclusive());
            testutil_assert(!(above_lower_key && below_upper_key));

            /*
             * Optimization to early exit, if we have traversed past all possible records in the
             * range bound.
             */
            if (!below_upper_key)
                break;

            ret = normal_cursor->next(normal_cursor.get());
        }
    }

    void
    insert_operation(thread_worker *tc) override final
    {
        /* Each insert operation will insert new keys in the collections. */
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        uint32_t rollback_retries = 0;
        while (tc->running()) {

            collection &coll = tc->db.get_random_collection();
            scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
            tc->txn.try_begin();

            while (tc->txn.active() && tc->running()) {

                /* Generate a random key. */
                auto key = random_generator::instance().generate_random_string(tc->key_size);
                auto value =
                  random_generator::instance().generate_pseudo_random_string(tc->value_size);
                /* Insert a key/value pair. */
                if (tc->insert(cursor, coll.id, key, value)) {
                    if (tc->txn.can_commit()) {
                        /* We are not checking the result of commit as it is not necessary. */
                        if (tc->txn.commit())
                            rollback_retries = 0;
                        else
                            ++rollback_retries;
                    }
                } else {
                    tc->txn.rollback();
                    ++rollback_retries;
                }
                testutil_assert(rollback_retries < MAX_ROLLBACKS);

                /* Sleep the duration defined by the configuration. */
                tc->sleep();
            }

            /* Reset our cursor to avoid pinning content. */
            testutil_check(cursor->reset(cursor.get()));
        }
        /* Rollback any transaction that could not commit before the end of the test. */
        tc->txn.try_rollback();
    }

    void
    update_operation(thread_worker *tc) override final
    {
        /* Each update operation will update existing keys in the collections. */
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        uint32_t rollback_retries = 0;
        while (tc->running()) {

            collection &coll = tc->db.get_random_collection();
            scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
            scoped_cursor rnd_cursor =
              tc->session.open_scoped_cursor(coll.name, "next_random=true");
            tc->txn.try_begin();

            while (tc->txn.active() && tc->running()) {
                int ret = rnd_cursor->next(rnd_cursor.get());

                /* It is possible not to find anything if the collection is empty. */
                testutil_assert(ret == 0 || ret == WT_NOTFOUND || ret == WT_ROLLBACK);
                if (ret == WT_NOTFOUND) {
                    /*
                     * If we cannot find any record, finish the current transaction as we might be
                     * able to see new records after starting a new one.
                     */
                    WT_IGNORE_RET_BOOL(tc->txn.commit());
                    continue;
                } else if (ret == WT_ROLLBACK)
                    break;

                const char *key;
                testutil_check(rnd_cursor->get_key(rnd_cursor.get(), &key));

                /* Update the found key with a randomized value. */
                auto value =
                  random_generator::instance().generate_pseudo_random_string(tc->value_size);
                if (tc->update(cursor, coll.id, key, value)) {
                    if (tc->txn.can_commit()) {
                        /* We are not checking the result of commit as it is not necessary. */
                        if (tc->txn.commit())
                            rollback_retries = 0;
                        else
                            ++rollback_retries;
                    }
                } else {
                    tc->txn.rollback();
                    ++rollback_retries;
                }
                testutil_assert(rollback_retries < MAX_ROLLBACKS);

                /* Sleep the duration defined by the configuration. */
                tc->sleep();
            }

            /* Reset our cursors to avoid pinning content. */
            testutil_check(rnd_cursor->reset(rnd_cursor.get()));
            testutil_check(cursor->reset(cursor.get()));
        }

        /* Rollback any transaction that could not commit before the end of the test. */
        tc->txn.try_rollback();
    }

    void
    read_operation(thread_worker *tc) override final
    {
        /*
         * Each read operation will perform search nears with a range bounded cursor and a normal
         * cursor without any bounds set. The normal cursor will be used to validate the results
         * from the range cursor.
         */
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        std::map<uint64_t, scoped_cursor> cursors;
        while (tc->running()) {
            /* Get a random collection to work on. */
            collection &coll = tc->db.get_random_collection();

            /* Find a cached cursor or create one if none exists. */
            if (cursors.find(coll.id) == cursors.end())
                cursors.emplace(coll.id, std::move(tc->session.open_scoped_cursor(coll.name)));

            /* Set random bounds on cached range cursor. */
            auto &bounded_cursor = cursors[coll.id];
            auto bound_pair = set_random_bounds(tc, bounded_cursor);

            scoped_cursor normal_cursor = tc->session.open_scoped_cursor(coll.name);
            wt_timestamp_t ts = tc->tsm->get_valid_read_ts();
            /*
             * The oldest timestamp might move ahead and the reading timestamp might become invalid.
             * To tackle this issue, we round the timestamp to the oldest timestamp value.
             */
            tc->txn.try_begin(
              "roundup_timestamps=(read=true),read_timestamp=" + tc->tsm->decimal_to_hex(ts));
            while (tc->txn.active() && tc->running()) {
                /* Generate a random string. */
                auto key_size = random_generator::instance().generate_integer(
                  static_cast<int64_t>(1), tc->key_size);
                auto srch_key = random_generator::instance().generate_random_string(
                  key_size, characters_type::ALPHABET);

                int exact = 0;
                bounded_cursor->set_key(bounded_cursor.get(), srch_key.c_str());
                auto ret = bounded_cursor->search_near(bounded_cursor.get(), &exact);
                testutil_assert(ret == 0 || ret == WT_NOTFOUND || ret == WT_ROLLBACK);
                if (ret == WT_ROLLBACK)
                    continue;

                /* Verify the bound search_near result using the normal cursor. */
                validate_bound_search_near(
                  ret, exact, bounded_cursor, normal_cursor, srch_key, bound_pair);

                /*
                 * If search near was successful, use the key it's currently positioned on as the
                 * search key to validate bound search.
                 */
                if (ret == 0) {
                    const char *range_srch_key;
                    testutil_check(bounded_cursor->get_key(bounded_cursor.get(), &range_srch_key));
                    /*
                     * Take a copy of the range search key as it the buffer returned by WiredTiger
                     * won't be valid after the call to set_key.
                     */
                    std::string range_key_copy(range_srch_key);
                    bounded_cursor->set_key(bounded_cursor.get(), range_key_copy.c_str());
                    ret = bounded_cursor->search(bounded_cursor.get());

                    testutil_assert(ret == 0 || ret == WT_NOTFOUND || ret == WT_ROLLBACK);
                    if (ret == WT_ROLLBACK)
                        continue;
                    validate_bound_search(ret, bounded_cursor, range_key_copy, bound_pair);
                }

                tc->txn.add_op();
                if (tc->txn.can_commit())
                    tc->txn.commit();
                tc->sleep();
            }
            bounded_cursor->reset(bounded_cursor.get());
            normal_cursor->reset(normal_cursor.get());
        }
        /* Roll back the last transaction if still active now the work is finished. */
        tc->txn.try_rollback();
    }

    void
    custom_operation(thread_worker *tc) override final
    {
        /*
         * Each custom operation will use the range bounded cursor to traverse through existing keys
         * in the collection. The records will be validated against with the normal cursor to check
         * for any potential missing records.
         */
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        std::map<uint64_t, scoped_cursor> cursors;
        while (tc->running()) {
            /* Get a random collection to work on. */
            collection &coll = tc->db.get_random_collection();

            /* Find a cached cursor or create one if none exists. */
            if (cursors.find(coll.id) == cursors.end())
                cursors.emplace(coll.id, std::move(tc->session.open_scoped_cursor(coll.name)));

            /* Set random bounds on cached range cursor. */
            auto &bounded_cursor = cursors[coll.id];
            auto bound_pair = set_random_bounds(tc, bounded_cursor);

            scoped_cursor normal_cursor = tc->session.open_scoped_cursor(coll.name);
            wt_timestamp_t ts = tc->tsm->get_valid_read_ts();
            /*
             * The oldest timestamp might move ahead and the reading timestamp might become invalid.
             * To tackle this issue, we round the timestamp to the oldest timestamp value.
             */
            tc->txn.begin(
              "roundup_timestamps=(read=true),read_timestamp=" + tc->tsm->decimal_to_hex(ts));
            while (tc->txn.active() && tc->running()) {
                cursor_traversal(bounded_cursor, normal_cursor, bound_pair.get_lower(),
                  bound_pair.get_upper(), true);
                testutil_check(normal_cursor->reset(normal_cursor.get()));
                cursor_traversal(bounded_cursor, normal_cursor, bound_pair.get_lower(),
                  bound_pair.get_upper(), false);
                tc->txn.add_op();
                tc->txn.try_rollback();
                tc->sleep();
            }
            normal_cursor->reset(normal_cursor.get());
        }
        /* Roll back the last transaction if still active now the work is finished. */
        tc->txn.try_rollback();
    }
};
