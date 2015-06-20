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

#include "mongo/base/data_type_terminated.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(DataTypeTerminated, Basic) {
    char buf[100];
    char a[] = "a";
    char b[] = "bb";
    char c[] = "ccc";

    {
        DataRangeCursor drc(buf, buf + sizeof(buf));
        ConstDataRange cdr_a(a, a + sizeof(a) + -1);
        ConstDataRange cdr_b(b, b + sizeof(b) + -1);
        ConstDataRange cdr_c(c, c + sizeof(c) + -1);

        ASSERT_OK(drc.writeAndAdvance(Terminated<'\0', ConstDataRange>(cdr_a)));
        ASSERT_OK(drc.writeAndAdvance(Terminated<'\0', ConstDataRange>(cdr_b)));
        ASSERT_OK(drc.writeAndAdvance(Terminated<'\0', ConstDataRange>(cdr_c)));

        ASSERT_EQUALS(1 + 2 + 3 + 3, drc.data() - buf);
    }

    {
        ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));

        Terminated<'\0', ConstDataRange> tcdr;

        ASSERT_OK(cdrc.readAndAdvance(&tcdr));
        ASSERT_EQUALS(std::string(a), tcdr.value.data());

        ASSERT_OK(cdrc.readAndAdvance(&tcdr));
        ASSERT_EQUALS(std::string(b), tcdr.value.data());

        ASSERT_OK(cdrc.readAndAdvance(&tcdr));
        ASSERT_EQUALS(std::string(c), tcdr.value.data());
    }
}

}  // namespace mongo
