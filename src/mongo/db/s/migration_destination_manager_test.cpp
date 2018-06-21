/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

class MigrationDestinationManagerTest : public ShardServerTestFixture {
protected:
    /**
     * Instantiates a BSON object in which both "_id" and "X" are set to value.
     */
    static BSONObj createDocument(int value) {
        return BSON("_id" << value << "X" << value);
    }

    /**
     * Creates a list of documents to clone.
     */
    static std::vector<BSONObj> createDocumentsToClone() {
        return {createDocument(1), createDocument(2), createDocument(3)};
    }

    /**
     * Creates a list of documents to clone and converts it to a BSONArray.
     */
    static BSONArray createDocumentsToCloneArray() {
        BSONArrayBuilder arrayBuilder;
        for (auto& doc : createDocumentsToClone()) {
            arrayBuilder.append(doc);
        }
        return arrayBuilder.arr();
    }
};

// Tests that documents will ferry from the fetch logic to the insert logic successfully.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsFromDonorWorksCorrectly) {
    bool ranOnce = false;

    auto fetchBatchFn = [&](OperationContext* opCtx) {
        BSONObjBuilder fetchBatchResultBuilder;

        if (ranOnce) {
            fetchBatchResultBuilder.append("objects", BSONObj());
        } else {
            ranOnce = true;
            fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());
        }

        return fetchBatchResultBuilder.obj();
    };

    std::vector<BSONObj> resultDocs;

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        for (auto&& docToClone : docs) {
            resultDocs.push_back(docToClone.Obj().getOwned());
        }
    };

    MigrationDestinationManager::cloneDocumentsFromDonor(
        operationContext(), insertBatchFn, fetchBatchFn);

    std::vector<BSONObj> originalDocs = createDocumentsToClone();

    ASSERT_EQ(originalDocs.size(), resultDocs.size());

    for (auto originalDocsIt = originalDocs.begin(), resultDocsIt = resultDocs.begin();
         originalDocsIt != originalDocs.end() && resultDocsIt != resultDocs.end();
         ++originalDocsIt, ++resultDocsIt) {
        ASSERT_BSONOBJ_EQ(*originalDocsIt, *resultDocsIt);
    }
}

// Tests that an exception in the fetch logic will successfully throw an exception on the main
// thread.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsThrowsFetchErrors) {
    bool ranOnce = false;

    auto fetchBatchFn = [&](OperationContext* opCtx) {
        BSONObjBuilder fetchBatchResultBuilder;

        if (ranOnce) {
            uasserted(ErrorCodes::NetworkTimeout, "network error");
        }

        ranOnce = true;
        fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());

        return fetchBatchResultBuilder.obj();
    };

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {};

    ASSERT_THROWS_CODE_AND_WHAT(MigrationDestinationManager::cloneDocumentsFromDonor(
                                    operationContext(), insertBatchFn, fetchBatchFn),
                                DBException,
                                ErrorCodes::NetworkTimeout,
                                "network error");
}

// Tests that an exception in the insertion logic will successfully throw an exception on the
// main thread.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsCatchesInsertErrors) {
    auto fetchBatchFn = [&](OperationContext* opCtx) {
        BSONObjBuilder fetchBatchResultBuilder;
        fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());
        return fetchBatchResultBuilder.obj();
    };

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        uasserted(ErrorCodes::FailedToParse, "insertion error");
    };

    // Since the error is thrown on another thread, the message becomes "operation was interrupted"
    // on the main thread.

    ASSERT_THROWS_CODE_AND_WHAT(MigrationDestinationManager::cloneDocumentsFromDonor(
                                    operationContext(), insertBatchFn, fetchBatchFn),
                                DBException,
                                ErrorCodes::FailedToParse,
                                "operation was interrupted");

    ASSERT_EQ(operationContext()->getKillStatus(), ErrorCodes::FailedToParse);
}

}  // namespace
}  // namespace mongo
