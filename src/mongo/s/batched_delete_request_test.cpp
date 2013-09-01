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

#include "mongo/s/batched_delete_request.h"

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/batched_delete_document.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONArray;
    using mongo::BSONObj;
    using mongo::BatchedDeleteRequest;
    using mongo::BatchedDeleteDocument;
    using mongo::BSONArrayBuilder;
    using mongo::OID;
    using mongo::OpTime;
    using std::string;


    TEST(RoundTrip, Normal) {
        BSONArray deleteArray =
            BSON_ARRAY(
                BSON(BatchedDeleteDocument::query(BSON("a" << 1)) <<
                     BatchedDeleteDocument::limit(1)
                    ) <<
                BSON(BatchedDeleteDocument::query(BSON("b" << 1)) <<
                     BatchedDeleteDocument::limit(1)
                    )
                );

        BSONObj writeConcernObj = BSON("w" << 1);

        // The BSON_ARRAY macro doesn't support Timestamps.
        BSONArrayBuilder arrBuilder;
        arrBuilder.appendTimestamp(OpTime(1,1).asDate());
        arrBuilder.append(OID::gen());
        BSONArray shardVersionArray = arrBuilder.arr();

        BSONObj origDeleteRequestObj =
            BSON(BatchedDeleteRequest::collName("test") <<
                 BatchedDeleteRequest::deletes() << deleteArray <<
                 BatchedDeleteRequest::writeConcern(writeConcernObj) <<
                 BatchedDeleteRequest::continueOnError(false) <<
                 BatchedDeleteRequest::shardVersion() << shardVersionArray <<
                 BatchedDeleteRequest::session(0));

        string errMsg;
        BatchedDeleteRequest request;
        bool ok = request.parseBSON(origDeleteRequestObj, &errMsg);
        ASSERT_TRUE(ok);

        BSONObj genDeleteRequestObj = request.toBSON();
        ASSERT_EQUALS(0, genDeleteRequestObj.woCompare(origDeleteRequestObj));
    }

} // unnamed namespace
