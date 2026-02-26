/*
 * Copyright (c) 2017, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "rnp_tests.h"
#include "support.h"
#include <rnp/rnpcfg.h>
#include "time-utils.h"

TEST_F(rnp_tests, test_rnpcfg)
{
    rnp_cfg  cfg1, cfg2;
    rnp_cfg *cfgs[2] = {&cfg1, &cfg2};
    rnp_cfg *cfg = NULL;
    char     buf[32];

    assert_null(cfg1.get_cstr("key"));

    /* set the values */
    cfg1.set_str("key_str", "val");
    cfg1.set_str("key_true", "true");
    cfg1.set_str("key_True", "True");
    cfg1.set_int("key_int", 999);
    cfg1.set_str("key_100", "100");
    cfg1.set_bool("key_bool", true);

    for (int i = 0; i < 10; i++) {
        snprintf(buf, sizeof(buf), "val%d", i);
        cfg1.add_str("key_list", buf);
    }

    /* copy empty cfg2 to cfg1 to make sure values are not deleted */
    cfg1.copy(cfg2);

    /* copy to the cfg2 */
    cfg2.copy(cfg1);

    /* copy second time to make sure there are no leaks */
    cfg2.copy(cfg1);

    /* get values back, including transformations */
    for (int i = 0; i < 2; i++) {
        cfg = cfgs[i];

        assert_int_equal(cfg->get_int("key_int"), 999);
        assert_int_equal(cfg->get_int("key_100"), 100);
        assert_true(cfg->get_bool("key_int"));
        assert_true(cfg->get_bool("key_bool"));
        assert_true(cfg->get_bool("key_true"));
        assert_true(cfg->get_bool("key_True"));
        assert_false(cfg->get_bool("key_notfound"));

        assert_string_equal(cfg->get_cstr("key_str"), "val");
        assert_null(cfg->get_cstr("key_str1"));
        assert_null(cfg->get_cstr("key_st"));

        const auto &lst = cfg->get_list("key_list");
        assert_int_equal(lst.size(), 10);

        for (int j = 0; j < 10; j++) {
            const auto &li = lst[j];
            snprintf(buf, sizeof(buf), "val%d", j);
            assert_string_equal(buf, li.c_str());
        }
    }

    /* get values back as C++ strings */
    assert_true(cfg1.get_str("key_str") == "val");
    assert_true(cfg1.get_str("key_unknown") == "");
    assert_true(cfg1.get_str("") == "");
    assert_true(cfg1.get_str("key_true") == "true");
    assert_true(cfg1.get_str("key_True") == "True");

    /* get C++ string list */
    auto keylist = cfg1.get_list("key_list11");
    assert_int_equal(keylist.size(), 0);
    keylist = cfg1.get_list("key_list");
    assert_int_equal(keylist.size(), 10);
    keylist = {"1", "2", "3"};
    keylist = cfg1.get_list("key_list");
    assert_int_equal(keylist.size(), 10);

    /* override value */
    cfg1.set_int("key_int", 222);
    assert_int_equal(cfg1.get_int("key_int"), 222);
    assert_int_equal(cfg2.get_int("key_int"), 999);
    cfg1.set_str("key_int", "333");
    assert_int_equal(cfg1.get_int("key_int"), 333);

    /* unset value */
    cfg1.unset("key_int");
}

TEST_F(rnp_tests, test_rnpcfg_get_expiration)
{
    time_t    basetime = time(NULL);
    time_t    rawtime = basetime + 604800;
    struct tm timeinfo;
    rnp_localtime(rawtime, timeinfo);
    // clear hours, minutes and seconds
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    rawtime = mktime(&timeinfo);
    auto    year = timeinfo.tm_year + 1900;
    auto    mon = timeinfo.tm_mon + 1;
    auto    day = timeinfo.tm_mday;
    rnp_cfg cfg;
    cfg.set_str("expiry-", fmt("%d-%02d-%02d", year, mon, day));
    cfg.set_str("expiry/", fmt("%d/%02d/%02d", year, mon, day));
    cfg.set_str("expiry.", fmt("%d.%02d.%02d", year, mon, day));

    uint32_t raw_expiry = 0;
    assert_true(cfg.get_expiration("expiry-", raw_expiry));
    assert_true(raw_expiry <= (rawtime - basetime));
    assert_true((rawtime - basetime) - raw_expiry <= 3);
    assert_true(cfg.get_expiration("expiry/", raw_expiry));
    assert_true(raw_expiry <= (rawtime - basetime));
    assert_true((rawtime - basetime) - raw_expiry <= 3);
    assert_true(cfg.get_expiration("expiry.", raw_expiry));
    assert_true(raw_expiry <= (rawtime - basetime));
    assert_true((rawtime - basetime) - raw_expiry <= 3);
    cfg.set_str("expiry", "2100-01-01");
    assert_true(cfg.get_expiration("expiry", raw_expiry));
    assert_int_not_equal(raw_expiry, rawtime - basetime);
    cfg.set_str("expiry", "2124-02-29");
    assert_true(cfg.get_expiration("expiry", raw_expiry));
    /* date in a past */
    cfg.set_str("expiry", "2000-02-29");
    assert_false(cfg.get_expiration("expiry", raw_expiry));
    cfg.set_str("expiry", "2400-02-29");
    if ((sizeof(time_t) > 4)) {
        /* date is correct, but overflows 32 bits */
        assert_false(cfg.get_expiration("expiry", raw_expiry));
    } else {
        /* for 32-bit time_t we return INT32_MAX for all dates beyond the y2038 */
        assert_true(cfg.get_expiration("expiry", raw_expiry));
        assert_int_equal(raw_expiry, INT32_MAX - basetime);
    }
    cfg.set_str("expiry", "2100-02-29");
    assert_false(cfg.get_expiration("expiry", raw_expiry));
    cfg.set_str("expiry", "4294967296");
    assert_false(cfg.get_expiration("expiry", raw_expiry));
    cfg.set_str("expiry", "2037-02-29");
    assert_false(cfg.get_expiration("expiry", raw_expiry));
    cfg.set_str("expiry", "2037-13-01");
    assert_false(cfg.get_expiration("expiry", raw_expiry));
}
