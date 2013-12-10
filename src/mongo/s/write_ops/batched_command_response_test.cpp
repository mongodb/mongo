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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/s/write_ops/batched_command_response.h"

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/write_ops/batched_error_detail.h"
#include "mongo/platform/cstdint.h"
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
