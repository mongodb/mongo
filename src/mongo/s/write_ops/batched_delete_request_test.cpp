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
#include "mongo/s/write_ops/batched_delete_document.h"
#include "mongo/s/write_ops/batched_delete_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

namespace {

TEST(BatchedDeleteRequest, Basic) {
    BSONArray deleteArray = BSON_ARRAY(
        BSON(BatchedDeleteDocument::query(BSON("a" << 1)) << BatchedDeleteDocument::limit(1))
        << BSON(BatchedDeleteDocument::query(BSON("b" << 1)) << BatchedDeleteDocument::limit(1)));

    BSONObj origDeleteRequestObj = BSON(
        BatchedDeleteRequest::collName("test") << BatchedDeleteRequest::deletes() << deleteArray
                                               << BatchedDeleteRequest::writeConcern(BSON("w" << 1))
                                               << BatchedDeleteRequest::ordered(true));

    string errMsg;
    BatchedDeleteRequest request;
    ASSERT_TRUE(request.parseBSON("foo", origDeleteRequestObj, &errMsg));

    ASSERT_EQ("foo.test", request.getNS().ns());

    ASSERT_EQUALS(origDeleteRequestObj, request.toBSON());
}

TEST(BatchedDeleteRequest, CloneBatchedDeleteDocCopiesAllFields) {
    BatchedDeleteDocument deleteDoc;
    deleteDoc.setQuery(BSON("a" << 1));
    deleteDoc.setLimit(1);
    deleteDoc.setCollation(BSON("locale"
                                << "en_US"));

    BatchedDeleteDocument cloneToDoc;
    deleteDoc.cloneTo(&cloneToDoc);
    ASSERT_TRUE(cloneToDoc.isQuerySet());
    ASSERT_EQ(BSON("a" << 1), cloneToDoc.getQuery());
    ASSERT_TRUE(cloneToDoc.isLimitSet());
    ASSERT_EQ(1, cloneToDoc.getLimit());
    ASSERT_TRUE(cloneToDoc.isCollationSet());
    ASSERT_EQ(BSON("locale"
                   << "en_US"),
              cloneToDoc.getCollation());
}

TEST(BatchedDeleteRequest, CanSetAndRetrieveCollationField) {
    BatchedDeleteDocument deleteDoc;
    deleteDoc.setQuery(BSON("a" << 1));
    deleteDoc.setLimit(1);

    ASSERT_FALSE(deleteDoc.isCollationSet());
    deleteDoc.setCollation(BSON("locale"
                                << "en_US"));
    ASSERT_TRUE(deleteDoc.isCollationSet());
    ASSERT_EQ(BSON("locale"
                   << "en_US"),
              deleteDoc.getCollation());
    deleteDoc.unsetCollation();
    ASSERT_FALSE(deleteDoc.isCollationSet());
}

TEST(BatchedDeleteRequest, ClearBatchedDeleteDocUnsetsCollation) {
    BatchedDeleteDocument deleteDoc;
    deleteDoc.setQuery(BSON("a" << 1));
    deleteDoc.setLimit(1);
    deleteDoc.setCollation(BSON("locale"
                                << "en_US"));

    ASSERT_TRUE(deleteDoc.isCollationSet());
    deleteDoc.clear();
    ASSERT_FALSE(deleteDoc.isCollationSet());
}

TEST(BatchedDeleteRequest, CollationFieldSerializesToBSONCorrectly) {
    BatchedDeleteDocument deleteDoc;
    deleteDoc.setQuery(BSON("a" << 1));
    deleteDoc.setLimit(1);
    deleteDoc.setCollation(BSON("locale"
                                << "en_US"));

    BSONObj expectedDeleteObj = BSON(BatchedDeleteDocument::query(BSON("a" << 1))
                                     << BatchedDeleteDocument::limit(1)
                                     << BatchedDeleteDocument::collation(BSON("locale"
                                                                              << "en_US")));
    ASSERT_EQUALS(expectedDeleteObj, deleteDoc.toBSON());
}

TEST(BatchedDeleteRequest, CollationFieldParsesFromBSONCorrectly) {
    BSONArray deleteArray = BSON_ARRAY(BSON(BatchedDeleteDocument::query(BSON("a" << 1))
                                            << BatchedDeleteDocument::limit(1)
                                            << BatchedDeleteDocument::collation(BSON("locale"
                                                                                     << "en_US"))));

    BSONObj origDeleteRequestObj = BSON(
        BatchedDeleteRequest::collName("test") << BatchedDeleteRequest::deletes() << deleteArray);

    std::string errMsg;
    BatchedDeleteRequest request;
    ASSERT_TRUE(request.parseBSON("foo", origDeleteRequestObj, &errMsg));

    ASSERT_EQ(1U, request.sizeDeletes());
    ASSERT_TRUE(request.getDeletesAt(0)->isCollationSet());
    ASSERT_EQ(BSON("locale"
                   << "en_US"),
              request.getDeletesAt(0)->getCollation());

    // Ensure we re-serialize to the original BSON request.
    ASSERT_EQUALS(origDeleteRequestObj, request.toBSON());
}

}  // namespace
}  // namespace mongo
