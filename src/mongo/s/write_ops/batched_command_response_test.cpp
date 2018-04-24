/**
 *    Copyright (C) 2013-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

namespace {

TEST(BatchedCommandResponse, Basic) {
    BSONArray writeErrorsArray = BSON_ARRAY(
        BSON(WriteErrorDetail::index(0) << WriteErrorDetail::errCode(ErrorCodes::IndexNotFound)
                                        << WriteErrorDetail::errCodeName("IndexNotFound")
                                        << WriteErrorDetail::errMessage("index 0 failed")
                                        << WriteErrorDetail::errInfo(BSON("more info" << 1)))
        << BSON(WriteErrorDetail::index(1)
                << WriteErrorDetail::errCode(ErrorCodes::InvalidNamespace)
                << WriteErrorDetail::errCodeName("InvalidNamespace")
                << WriteErrorDetail::errMessage("index 1 failed too")
                << WriteErrorDetail::errInfo(BSON("more info" << 1))));

    BSONObj writeConcernError(
        BSON("code" << 8 << "codeName" << ErrorCodes::errorString(ErrorCodes::Error(8)) << "errmsg"
                    << "norepl"
                    << "errInfo"
                    << BSON("a" << 1)));

    BSONObj origResponseObj =
        BSON("ok" << 1.0 << BatchedCommandResponse::n(0) << "opTime" << mongo::Timestamp(1ULL)
                  << BatchedCommandResponse::writeErrors()
                  << writeErrorsArray
                  << BatchedCommandResponse::writeConcernError()
                  << writeConcernError);

    string errMsg;
    BatchedCommandResponse response;
    bool ok = response.parseBSON(origResponseObj, &errMsg);
    ASSERT_TRUE(ok);

    BSONObj genResponseObj = response.toBSON();
    ASSERT_EQUALS(0, genResponseObj.woCompare(origResponseObj))
        << "\nparsed:   " << genResponseObj  //
        << "\noriginal: " << origResponseObj;
}

TEST(BatchedCommandResponse, TooManySmallErrors) {
    BatchedCommandResponse response;

    const auto bigstr = std::string(1024, 'x');

    for (int i = 0; i < 100'000; i++) {
        auto errDetail = stdx::make_unique<WriteErrorDetail>();
        errDetail->setIndex(i);
        errDetail->setStatus({ErrorCodes::BadValue, bigstr});
        response.addToErrDetails(errDetail.release());
    }

    response.setStatus(Status::OK());
    const auto bson = response.toBSON();
    ASSERT_LT(bson.objsize(), BSONObjMaxUserSize);
    const auto errDetails = bson["writeErrors"].Array();
    ASSERT_EQ(errDetails.size(), 100'000u);

    for (int i = 0; i < 100'000; i++) {
        auto errDetail = errDetails[i].Obj();
        ASSERT_EQ(errDetail["index"].Int(), i);
        ASSERT_EQ(errDetail["code"].Int(), ErrorCodes::BadValue);

        if (i < 1024) {
            ASSERT_EQ(errDetail["errmsg"].String(), bigstr) << i;
        } else {
            ASSERT_EQ(errDetail["errmsg"].String(), ""_sd) << i;
        }
    }
}

TEST(BatchedCommandResponse, TooManyBigErrors) {
    BatchedCommandResponse response;

    const auto bigstr = std::string(2'000'000, 'x');
    const auto smallstr = std::string(10, 'x');

    for (int i = 0; i < 100'000; i++) {
        auto errDetail = stdx::make_unique<WriteErrorDetail>();
        errDetail->setIndex(i);
        errDetail->setStatus({ErrorCodes::BadValue,          //
                              i < 10 ? bigstr : smallstr});  // Don't waste too much RAM.
        response.addToErrDetails(errDetail.release());
    }

    response.setStatus(Status::OK());
    const auto bson = response.toBSON();
    ASSERT_LT(bson.objsize(), BSONObjMaxUserSize);
    const auto errDetails = bson["writeErrors"].Array();
    ASSERT_EQ(errDetails.size(), 100'000u);

    for (int i = 0; i < 100'000; i++) {
        auto errDetail = errDetails[i].Obj();
        ASSERT_EQ(errDetail["index"].Int(), i);
        ASSERT_EQ(errDetail["code"].Int(), ErrorCodes::BadValue);

        if (i < 2) {
            ASSERT_EQ(errDetail["errmsg"].String(), bigstr) << i;
        } else {
            ASSERT_EQ(errDetail["errmsg"].String(), ""_sd) << i;
        }
    }
}

}  // namespace
}  // namespace mongo
