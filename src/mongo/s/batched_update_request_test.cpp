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

#include "mongo/s/batched_update_request.h"

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/batched_update_document.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;
    using mongo::BatchedUpdateDocument;
    using mongo::BatchedUpdateRequest;
    using mongo::BSONArray;
    using mongo::BSONArrayBuilder;
    using mongo::BSONObj;
    using mongo::OID;
    using mongo::OpTime;

    TEST(RoundTrip, Normal) {
        BSONArray updateArray =
            BSON_ARRAY(
                BSON(BatchedUpdateDocument::query(BSON("a" << 1)) <<
                     BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("a" << 1))) <<
                     BatchedUpdateDocument::multi(false) <<
                     BatchedUpdateDocument::upsert(false)
                    ) <<
                BSON(BatchedUpdateDocument::query(BSON("b" << 1)) <<
                     BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("b" << 2))) <<
                     BatchedUpdateDocument::multi(false) <<
                     BatchedUpdateDocument::upsert(false)
                    )
                );

        BSONObj writeConcernObj = BSON("w" << 1);

        // The BSON_ARRAY macro doesn't support Timestamps.
        BSONArrayBuilder arrBuilder;
        arrBuilder.appendTimestamp(OpTime(1,1).asDate());
        arrBuilder.append(OID::gen());
        BSONArray shardVersionArray = arrBuilder.arr();

        BSONObj origUpdateRequestObj =
            BSON(BatchedUpdateRequest::collName("test") <<
                 BatchedUpdateRequest::updates() << updateArray <<
                 BatchedUpdateRequest::writeConcern(writeConcernObj) <<
                 BatchedUpdateRequest::continueOnError(false) <<
                 BatchedUpdateRequest::shardVersion() << shardVersionArray <<
                 BatchedUpdateRequest::session(0));

        string errMsg;
        BatchedUpdateRequest request;
        bool ok = request.parseBSON(origUpdateRequestObj, &errMsg);
        ASSERT_TRUE(ok);

        BSONObj genUpdateRequestObj = request.toBSON();
        ASSERT_EQUALS(0, genUpdateRequestObj.woCompare(origUpdateRequestObj));
    }

} // unnamed namespace
