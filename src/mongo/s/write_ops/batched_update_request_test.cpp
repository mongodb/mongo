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
#include "mongo/s/write_ops/batched_update_document.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

namespace {

TEST(BatchedUpdateRequest, Basic) {
    BSONArray updateArray = BSON_ARRAY(
        BSON(BatchedUpdateDocument::query(BSON("a" << 1))
             << BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("a" << 1)))
             << BatchedUpdateDocument::multi(false) << BatchedUpdateDocument::upsert(false))
        << BSON(BatchedUpdateDocument::query(BSON("b" << 1))
                << BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("b" << 2)))
                << BatchedUpdateDocument::multi(false) << BatchedUpdateDocument::upsert(false)));

    BSONObj writeConcernObj = BSON("w" << 1);

    // The BSON_ARRAY macro doesn't support Timestamps.
    BSONArrayBuilder arrBuilder;
    arrBuilder.append(Timestamp(1, 1));
    arrBuilder.append(OID::gen());
    BSONArray shardVersionArray = arrBuilder.arr();

    BSONObj origUpdateRequestObj =
        BSON(BatchedUpdateRequest::collName("test")
             << BatchedUpdateRequest::updates() << updateArray
             << BatchedUpdateRequest::writeConcern(writeConcernObj)
             << BatchedUpdateRequest::ordered(true) << BatchedUpdateRequest::metadata()
             << BSON(BatchedRequestMetadata::shardName("shard0000")
                     << BatchedRequestMetadata::shardVersion() << shardVersionArray
                     << BatchedRequestMetadata::session(0)));

    string errMsg;
    BatchedUpdateRequest request;
    bool ok = request.parseBSON("foo", origUpdateRequestObj, &errMsg);
    ASSERT_TRUE(ok);

    ASSERT_EQ("foo.test", request.getNS().ns());

    BSONObj genUpdateRequestObj = request.toBSON();
    ASSERT_EQUALS(0, genUpdateRequestObj.woCompare(origUpdateRequestObj));
}

}  // namespace
}  // namespace mongo
