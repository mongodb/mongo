// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_range_cursor.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>

namespace mongo {

TEST(BSONObjDataType, ConstDataTypeRangeBSON) {
    char buf[1000] = {0};

    DataRangeCursor drc(buf, buf + sizeof(buf));

    {
        BSONObjBuilder b;
        b.append("a", 1);

        ASSERT_OK(drc.writeAndAdvanceNoThrow(b.obj()));
    }
    {
        BSONObjBuilder b;
        b.append("b", "fooo");

        ASSERT_OK(drc.writeAndAdvanceNoThrow(b.obj()));
    }
    {
        BSONObjBuilder b;
        b.append("c", 3);

        ASSERT_OK(drc.writeAndAdvanceNoThrow(b.obj()));
    }

    ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));

    ASSERT_EQUALS(1, cdrc.readAndAdvance<BSONObj>().getField("a").numberInt());
    ASSERT_EQUALS("fooo", cdrc.readAndAdvance<BSONObj>().getField("b").str());
    ASSERT_EQUALS(3, cdrc.readAndAdvance<BSONObj>().getField("c").numberInt());
}

}  // namespace mongo
