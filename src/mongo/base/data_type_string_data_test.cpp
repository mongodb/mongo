/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_terminated.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <utility>

namespace mongo {

TEST(DataTypeStringData, Basic) {
    char buf[100];
    std::string_view a("a");
    std::string_view b("bb");
    std::string_view c("ccc");

    {
        DataRangeCursor drc(buf, buf + sizeof(buf));

        ASSERT_OK(drc.writeAndAdvanceNoThrow(Terminated<'\0', std::string_view>(a)));
        ASSERT_OK(drc.writeAndAdvanceNoThrow(Terminated<'\0', std::string_view>(b)));
        ASSERT_OK(drc.writeAndAdvanceNoThrow(Terminated<'\0', std::string_view>(c)));

        ASSERT_EQUALS(1 + 2 + 3 + 3, drc.data() - buf);
    }

    {
        ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));

        Terminated<'\0', std::string_view> tsd;

        ASSERT_OK(cdrc.readAndAdvanceNoThrow(&tsd));
        ASSERT_EQUALS(a, tsd.value);

        ASSERT_OK(cdrc.readAndAdvanceNoThrow(&tsd));
        ASSERT_EQUALS(b, tsd.value);

        ASSERT_OK(cdrc.readAndAdvanceNoThrow(&tsd));
        ASSERT_EQUALS(c, tsd.value);
    }
}

}  // namespace mongo
