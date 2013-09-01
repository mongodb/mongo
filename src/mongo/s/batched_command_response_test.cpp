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

#include "mongo/s/batched_command_response.h"

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/batched_error_detail.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONArray;
    using mongo::BSONObj;
    using mongo::BatchedCommandResponse;
    using mongo::BatchedErrorDetail;
    using mongo::Date_t;
    using std::string;

    TEST(RoundTrip, Normal) {

        BSONArray errDetailsArray =
            BSON_ARRAY(
                BSON(BatchedErrorDetail::index(0) <<
                     BatchedErrorDetail::errCode(-2) <<
                     BatchedErrorDetail::errInfo(BSON("more info" << 1)) <<
                     BatchedErrorDetail::errMessage("index 0 failed")
                    ) <<
                BSON(BatchedErrorDetail::index(1) <<
                     BatchedErrorDetail::errCode(-3) <<
                     BatchedErrorDetail::errInfo(BSON("more info" << 1)) <<
                     BatchedErrorDetail::errMessage("index 1 failed too")
                    )
                );

        BSONObj origResponseObj =
            BSON(BatchedCommandResponse::ok(false) <<
                 BatchedCommandResponse::errCode(-1) <<
                 BatchedCommandResponse::errInfo(BSON("moreInfo" << 1)) <<
                 BatchedCommandResponse::errMessage("this batch didn't work") <<
                 BatchedCommandResponse::n(0) <<
                 BatchedCommandResponse::upserted(false) <<
                 BatchedCommandResponse::lastOp(Date_t(1)) <<
                 BatchedCommandResponse::errDetails() << errDetailsArray);

        string errMsg;
        BatchedCommandResponse response;
        bool ok = response.parseBSON(origResponseObj, &errMsg);
        ASSERT_TRUE(ok);

        BSONObj genResponseObj = response.toBSON();
        ASSERT_EQUALS(0, genResponseObj.woCompare(origResponseObj));
    }

} // unnamed namespace
