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
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

namespace {

TEST(BatchedCommandResponse, Basic) {
    BSONArray writeErrorsArray = BSON_ARRAY(
        BSON(WriteErrorDetail::index(0) << WriteErrorDetail::errCode(-2)
                                        << WriteErrorDetail::errInfo(BSON("more info" << 1))
                                        << WriteErrorDetail::errMessage("index 0 failed"))
        << BSON(WriteErrorDetail::index(1) << WriteErrorDetail::errCode(-3)
                                           << WriteErrorDetail::errInfo(BSON("more info" << 1))
                                           << WriteErrorDetail::errMessage("index 1 failed too")));

    BSONObj writeConcernError(BSON("code" << 8 << "errInfo" << BSON("a" << 1) << "errmsg"
                                          << "norepl"));

    BSONObj origResponseObj = BSON(BatchedCommandResponse::ok(false)
                                   << BatchedCommandResponse::errCode(-1)
                                   << BatchedCommandResponse::errMessage("this batch didn't work")
                                   << BatchedCommandResponse::n(0)
                                   << "opTime"
                                   << mongo::Timestamp(1ULL)
                                   << BatchedCommandResponse::writeErrors()
                                   << writeErrorsArray
                                   << BatchedCommandResponse::writeConcernError()
                                   << writeConcernError);

    string errMsg;
    BatchedCommandResponse response;
    bool ok = response.parseBSON(origResponseObj, &errMsg);
    ASSERT_TRUE(ok);

    BSONObj genResponseObj = response.toBSON();
    ASSERT_EQUALS(0, genResponseObj.woCompare(origResponseObj)) << "parsed: " << genResponseObj
                                                                << " original: " << origResponseObj;
}

}  // namespace
}  // namespace mongo
