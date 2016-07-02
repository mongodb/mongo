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
    BSONArray updateArray =
        BSON_ARRAY(BSON(BatchedUpdateDocument::query(BSON("a" << 1))
                        << BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("a" << 1)))
                        << BatchedUpdateDocument::multi(false)
                        << BatchedUpdateDocument::upsert(false))
                   << BSON(BatchedUpdateDocument::query(BSON("b" << 1))
                           << BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("b" << 2)))
                           << BatchedUpdateDocument::multi(false)
                           << BatchedUpdateDocument::upsert(false)));

    BSONObj origUpdateRequestObj = BSON(
        BatchedUpdateRequest::collName("test") << BatchedUpdateRequest::updates() << updateArray
                                               << BatchedUpdateRequest::writeConcern(BSON("w" << 1))
                                               << BatchedUpdateRequest::ordered(true));

    string errMsg;
    BatchedUpdateRequest request;
    ASSERT_TRUE(request.parseBSON("foo", origUpdateRequestObj, &errMsg));

    ASSERT_EQ("foo.test", request.getNS().ns());

    ASSERT_EQUALS(origUpdateRequestObj, request.toBSON());
}

TEST(BatchedUpdateRequest, CloneBatchedUpdateDocCopiesAllFields) {
    BatchedUpdateDocument updateDoc;
    updateDoc.setQuery(BSON("a" << 1));
    updateDoc.setUpdateExpr(BSON("$set" << BSON("a" << 2)));
    updateDoc.setMulti(true);
    updateDoc.setUpsert(true);
    updateDoc.setCollation(BSON("locale"
                                << "en_US"));

    BatchedUpdateDocument cloneToDoc;
    updateDoc.cloneTo(&cloneToDoc);
    ASSERT_TRUE(cloneToDoc.isQuerySet());
    ASSERT_EQ(BSON("a" << 1), cloneToDoc.getQuery());
    ASSERT_TRUE(cloneToDoc.isUpdateExprSet());
    ASSERT_EQ(BSON("$set" << BSON("a" << 2)), cloneToDoc.getUpdateExpr());
    ASSERT_TRUE(cloneToDoc.isMultiSet());
    ASSERT_TRUE(cloneToDoc.getMulti());
    ASSERT_TRUE(cloneToDoc.isUpsertSet());
    ASSERT_TRUE(cloneToDoc.getUpsert());
    ASSERT_TRUE(cloneToDoc.isCollationSet());
    ASSERT_EQ(BSON("locale"
                   << "en_US"),
              cloneToDoc.getCollation());
}

TEST(BatchedUpdateRequest, CanSetAndRetrieveCollationField) {
    BatchedUpdateDocument updateDoc;
    updateDoc.setQuery(BSON("a" << 1));
    updateDoc.setUpdateExpr(BSON("$set" << BSON("a" << 2)));

    ASSERT_FALSE(updateDoc.isCollationSet());
    updateDoc.setCollation(BSON("locale"
                                << "en_US"));
    ASSERT_TRUE(updateDoc.isCollationSet());
    ASSERT_EQ(BSON("locale"
                   << "en_US"),
              updateDoc.getCollation());
    updateDoc.unsetCollation();
    ASSERT_FALSE(updateDoc.isCollationSet());
}

TEST(BatchedUpdateRequest, ClearBatchedUpdateDocUnsetsCollation) {
    BatchedUpdateDocument updateDoc;
    updateDoc.setQuery(BSON("a" << 1));
    updateDoc.setUpdateExpr(BSON("$set" << BSON("a" << 2)));
    updateDoc.setCollation(BSON("locale"
                                << "en_US"));

    ASSERT_TRUE(updateDoc.isCollationSet());
    updateDoc.clear();
    ASSERT_FALSE(updateDoc.isCollationSet());
}

TEST(BatchedUpdateRequest, CollationFieldSerializesToBSONCorrectly) {
    BatchedUpdateDocument updateDoc;
    updateDoc.setQuery(BSON("a" << 1));
    updateDoc.setUpdateExpr(BSON("$set" << BSON("a" << 2)));
    updateDoc.setCollation(BSON("locale"
                                << "en_US"));

    BSONObj expectedUpdateObj =
        BSON(BatchedUpdateDocument::query(BSON("a" << 1))
             << BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("a" << 2)))
             << BatchedUpdateDocument::collation(BSON("locale"
                                                      << "en_US")));

    ASSERT_EQUALS(expectedUpdateObj, updateDoc.toBSON());
}

TEST(BatchedUpdateRequest, CollationFieldParsesFromBSONCorrectly) {
    BSONArray updateArray =
        BSON_ARRAY(BSON(BatchedUpdateDocument::query(BSON("a" << 1))
                        << BatchedUpdateDocument::updateExpr(BSON("$set" << BSON("a" << 2)))
                        << BatchedUpdateDocument::collation(BSON("locale"
                                                                 << "en_US"))));

    BSONObj origUpdateRequestObj = BSON(
        BatchedUpdateRequest::collName("test") << BatchedUpdateRequest::updates() << updateArray);

    std::string errMsg;
    BatchedUpdateRequest request;
    ASSERT_TRUE(request.parseBSON("foo", origUpdateRequestObj, &errMsg));

    ASSERT_EQ(1U, request.sizeUpdates());
    ASSERT_TRUE(request.getUpdatesAt(0)->isCollationSet());
    ASSERT_EQ(BSON("locale"
                   << "en_US"),
              request.getUpdatesAt(0)->getCollation());

    // Ensure we re-serialize to the original BSON request.
    ASSERT_EQUALS(origUpdateRequestObj, request.toBSON());
}

}  // namespace
}  // namespace mongo
