// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
