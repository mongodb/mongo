/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

/*
 * __wti_evict_threshold_pct is the pure arithmetic behind the pct_full output of __wt_evict_needed:
 * one hundred minus the smallest margin between a usage percentage and its trigger. Testing it
 * directly, rather than through __wt_evict_needed, avoids needing a live connection whose
 * eviction server would race the test over the same cache accounting the test wants to control.
 */
TEST_CASE(
  "Eviction threshold percentage is at least one hundred once a trigger is exceeded", "[evict]")
{
    /* Triggers as percentages of the cache size. */
    const double clean_trigger = 95.0, dirty_trigger = 20.0, updates_trigger = 10.0;

    /*
     * Cache usage as a percentage of the cache size: clean, dirty leaf, updates. expected_pct is
     * the value __wti_evict_threshold_pct must return; expected_over is whether at least one usage
     * exceeds its trigger, which is exactly when the result is required to be at least one hundred.
     */
    struct {
        double pct_clean, pct_dirty, pct_updates;
        double expected_pct;
        bool expected_over;
    } cases[] = {
      {10.0, 1.0, 1.0, 91.0, false},   /* Nothing over trigger. */
      {94.0, 19.0, 9.0, 99.0, false},  /* Everything just under trigger. */
      {95.0, 19.0, 9.0, 100.0, false}, /* Clean exactly at trigger: not over, but pct hits 100. */
      {96.0, 1.0, 1.0, 101.0, true},   /* Clean over trigger only. */
      {50.0, 25.0, 1.0, 105.0, true},  /* Dirty over trigger only. */
      {50.0, 1.0, 15.0, 105.0, true},  /* Updates over trigger only. */
      {96.0, 25.0, 15.0, 105.0, true}, /* All three over trigger. */
    };

    for (auto &c : cases) {
        double pct_full = __wti_evict_threshold_pct(
          c.pct_clean, c.pct_dirty, c.pct_updates, clean_trigger, dirty_trigger, updates_trigger);

        INFO("pct_clean " << c.pct_clean << " pct_dirty " << c.pct_dirty << " pct_updates "
                          << c.pct_updates);
        REQUIRE(pct_full == Approx(c.expected_pct));

        bool over = c.pct_clean > clean_trigger || c.pct_dirty > dirty_trigger ||
          c.pct_updates > updates_trigger;
        REQUIRE(over == c.expected_over);

        /*
         * This is the invariant that made the application-thread eviction assist loop's per-call
         * progress cap unreachable: once any trigger is exceeded, the percentage this function
         * reports can never be below one hundred, so a caller cannot use it to distinguish degrees
         * of cache pressure.
         */
        if (over)
            REQUIRE(pct_full >= 100.0);
    }
}
