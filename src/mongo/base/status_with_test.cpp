/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {

using mongo::makeStatusWith;
using mongo::StatusWith;

TEST(StatusWith, makeStatusWith) {
    using mongo::StringData;

    auto s1 = makeStatusWith<int>(3);
    ASSERT_TRUE(s1.isOK());
    ASSERT_EQUALS(uassertStatusOK(s1), 3);

    auto s2 = makeStatusWith<std::vector<int>>();
    ASSERT_TRUE(s2.isOK());
    ASSERT_EQUALS(uassertStatusOK(s2).size(), 0u);

    std::vector<int> i = {1, 2, 3};
    auto s3 = makeStatusWith<std::vector<int>>(i.begin(), i.end());
    ASSERT_TRUE(s3.isOK());
    ASSERT_EQUALS(uassertStatusOK(s3).size(), 3u);

    auto s4 = makeStatusWith<std::string>("foo");

    ASSERT_TRUE(s4.isOK());
    ASSERT_EQUALS(uassertStatusOK(s4), std::string{"foo"});
    const char* foo = "barbaz";
    auto s5 = makeStatusWith<StringData>(foo, std::size_t{6});
    ASSERT_TRUE(s5.isOK());

    // make sure CV qualifiers trigger correct overload
    const StatusWith<StringData>& s6 = s5;
    ASSERT_EQUALS(uassertStatusOK(s6), foo);
    StatusWith<StringData>& s7 = s5;
    ASSERT_EQUALS(uassertStatusOK(s7), foo);
    ASSERT_EQUALS(uassertStatusOK(std::move(s5)), foo);

    // Check that we use T(...) and not T{...}
    // ASSERT_EQUALS requires an ostream overload for vector<int>
    ASSERT_TRUE(makeStatusWith<std::vector<int>>(1, 2) == std::vector<int>{2});
}

TEST(StatusWith, nonDefaultConstructible) {
    class NoDefault {
        NoDefault() = delete;

    public:
        NoDefault(int x) : _x{x} {};
        int _x{0};
    };

    auto swND = makeStatusWith<NoDefault>(1);
    ASSERT_TRUE(swND.getValue()._x = 1);

    auto swNDerror = StatusWith<NoDefault>(mongo::ErrorCodes::BadValue, "foo");
    ASSERT_FALSE(swNDerror.isOK());
}

}  // namespace
