/**
 *    Copyright 2017 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/rollback_fix_up_info_descriptions.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

TEST(RollbackFixUpInfoDescriptionsTest, SingleDocumentDescriptionToBson) {
    auto collectionUuid = UUID::gen();
    auto docId = BSON("_id"
                      << "mydocid");

    RollbackFixUpInfo::SingleDocumentOperationDescription description(
        collectionUuid, docId.firstElement(), RollbackFixUpInfo::SingleDocumentOpType::kInsert);

    auto expectedDocument = BSON(
        "_id" << BSON("collectionUuid" << collectionUuid.toBSON().firstElement() << "documentId"
                                       << docId.firstElement())
              << "operationType"
              << "insert"
              << "documentToRestore"
              << BSONNULL);

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

TEST(RollbackFixUpInfoDescriptionsTest, CollectionUuidDescriptionToBson) {
    auto collectionUuid = UUID::gen();
    NamespaceString nss("mydb.mylostcoll");

    RollbackFixUpInfo::CollectionUuidDescription description(collectionUuid, nss);

    auto expectedDocument =
        BSON("_id" << collectionUuid.toBSON().firstElement() << "ns" << nss.ns());

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

TEST(RollbackFixUpInfoDescriptionsTest, CollectionUuidDescriptionWithEmptyNamespaceToBson) {
    auto collectionUuid = UUID::gen();
    NamespaceString emptyNss;

    RollbackFixUpInfo::CollectionUuidDescription description(collectionUuid, emptyNss);

    auto expectedDocument = BSON("_id" << collectionUuid.toBSON().firstElement() << "ns"
                                       << "");

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

}  // namespace
