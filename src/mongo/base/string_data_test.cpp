/**
 *    Copyright (C) 2012 10gen Inc.
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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::StringData;
    using std::string;

    TEST(Construction, FromStdString) {
        std::string base("aaa");
        StringData strData(base);
        ASSERT_EQUALS(strData.size(), base.size());
        ASSERT_EQUALS(strData.toString(), base);
    }

    TEST(Construction, FromCString) {
        std::string base("aaa");
        StringData strData(base.c_str());
        ASSERT_EQUALS(strData.size(), base.size());
        ASSERT_EQUALS(strData.toString(), base);
    }

    TEST(Construction, FromLiteral) {
        StringData strData("ccc", StringData::LiteralTag());
        ASSERT_EQUALS(strData.size(), 3U);
        ASSERT_EQUALS(strData.toString(), string("ccc"));
    }

    TEST(Comparison, BothEmpty) {
        StringData empty("");
        ASSERT_TRUE(empty == empty);
        ASSERT_FALSE(empty != empty);
        ASSERT_FALSE(empty > empty);
        ASSERT_TRUE(empty >= empty);
        ASSERT_FALSE(empty < empty);
        ASSERT_TRUE(empty <= empty);
    }

    TEST(Comparison, BothNonEmptyOnSize) {
        StringData a("a");
        StringData aa("aa");
        ASSERT_FALSE(a == aa);
        ASSERT_TRUE(a != aa);
        ASSERT_FALSE(a > aa);
        ASSERT_FALSE(a >= aa);
        ASSERT_TRUE(a >= a);
        ASSERT_TRUE(a < aa);
        ASSERT_TRUE(a <= aa);
        ASSERT_TRUE(a <= a);
    }

    TEST(Comparison, BothNonEmptyOnContent) {
        StringData a("a");
        StringData b("b");
        ASSERT_FALSE(a == b);
        ASSERT_TRUE(a != b);
        ASSERT_FALSE(a > b);
        ASSERT_FALSE(a >= b);
        ASSERT_TRUE(a < b);
        ASSERT_TRUE(a <= b);
    }

    TEST(Comparison, MixedEmptyAndNot) {
        StringData empty("");
        StringData a("a");
        ASSERT_FALSE(a == empty);
        ASSERT_TRUE(a != empty);
        ASSERT_TRUE(a > empty);
        ASSERT_TRUE(a >= empty);
        ASSERT_FALSE(a < empty);
        ASSERT_FALSE(a <= empty);
    }

} // unnamed namespace
