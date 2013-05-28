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
 */

#include "mongo/db/ops/update_driver.h"

#include "mongo/unittest/unittest.h"
#include "mongo/db/json.h"

namespace {

    using mongo::fromjson;
    using mongo::UpdateDriver;

    TEST(Parse, Normal) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1}}")));
        ASSERT_EQUALS(driver.numMods(), 1U);
    }

    TEST(Parse, MultiMods) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{a:1, b:1}}")));
        ASSERT_EQUALS(driver.numMods(), 2U);
    }

    TEST(Parse, EmptyMods) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:{}}")));
    }

    TEST(Parse, WrongType) {
        UpdateDriver::Options opts;
        UpdateDriver driver(opts);
        ASSERT_OK(driver.parse(fromjson("{$set:[{a:1}]}")));
    }

} // unnamed namespace
