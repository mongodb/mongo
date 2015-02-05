// str_test.cpp

/*    Copyright 2014 MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

    namespace str = mongoutils::str;

    TEST(StripTrailingTests, RemoveFromHead) {
        std::string data("remove from head");
        str::stripTrailing(data, "re");
        ASSERT_EQUALS("mov fom had", data);
    }

    TEST(StripTrailingTests, RemoveFromTail) {
        std::string data("remove from tail");
        str::stripTrailing(data, "ail");
        ASSERT_EQUALS("remove from t", data);
    }

    TEST(StripTrailingTests, RemoveSpaces) {
        std::string data("remove spaces");
        str::stripTrailing(data, " a");
        ASSERT_EQUALS("removespces", data);
    }

    TEST(StripTrailingTests, RemoveFromMiddle) {
        std::string data("remove from middle");
        str::stripTrailing(data, "from");
        ASSERT_EQUALS("eve  iddle", data);
    }

    TEST(StripTrailingTests, RemoveFromEmpty) {
        std::string data("");
        str::stripTrailing(data, "from");
        ASSERT_EQUALS("", data);
    }

    TEST(StripTrailingTests, RemoveEmpty) {
        std::string data("remove empty");
        str::stripTrailing(data, "");
        ASSERT_EQUALS("remove empty", data);
    }

    TEST(StripTrailingTests, RemoveBringsEmptyResult) {
        std::string data("remove");
        str::stripTrailing(data, "remove");
        ASSERT_EQUALS("", data);
    }

}  // namespace
