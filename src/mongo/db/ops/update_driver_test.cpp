/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/ops/update_driver.h"

#include "mongo/db/index_set.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::fromjson;
    using mongo::IndexPathSet;
    using mongo::UpdateDriver;

    TEST(Parse, Normal) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1}}")));
        ASSERT_EQUALS(driver.numMods(), 1U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, MultiMods) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1, b:1}}")));
        ASSERT_EQUALS(driver.numMods(), 2U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, MixingMods) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1}, $unset:{b:1}}")));
        ASSERT_EQUALS(driver.numMods(), 2U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, ObjectReplacment) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{obj: \"obj replacement\"}")));
        ASSERT_TRUE(driver.isDocReplacement());
    }

    TEST(Parse, EmptyMod) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$set:{}}")));
    }

    TEST(Parse, WrongMod) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$xyz:{a:1}}")));
    }

    TEST(Parse, WrongType) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$set:[{a:1}]}")));
    }

    TEST(Parse, ModsWithLaterObjReplacement)  {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_NOT_OK(driver.parse(fromjson("{$set:{a:1}, obj: \"obj replacement\"}")));
    }

    TEST(Parse, PushAll) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$pushAll:{a:[1,2,3]}}")));
        ASSERT_EQUALS(driver.numMods(), 1U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

    TEST(Parse, SetOnInsert) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$setOnInsert:{a:1}}")));
        ASSERT_EQUALS(driver.numMods(), 1U);
        ASSERT_FALSE(driver.isDocReplacement());
    }

} // unnamed namespace
