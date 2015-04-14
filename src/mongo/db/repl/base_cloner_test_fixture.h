/**
 *    Copyright 2015 MongoDB Inc.
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

#pragma once

#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

    struct CollectionOptions;
    class OperationContext;

namespace repl {

    class BaseCloner;

    class BaseClonerTest : public ReplicationExecutorTest {
    public:
        typedef NetworkInterfaceMock::NetworkOperationIterator NetworkOperationIterator;

        /**
         * Creates an initial error status suitable for checking if
         * cloner has modified the 'status' field in test fixture.
         */
        static Status getDefaultStatus();

        /**
         * Creates a cursor response with given array of documents.
         */
        static BSONObj createCursorResponse(CursorId cursorId,
                                            const std::string& ns,
                                            const BSONArray& docs,
                                            const char* batchFieldName);

        static BSONObj createCursorResponse(CursorId cursorId,
                                            const BSONArray& docs,
                                            const char* batchFieldName);

        static BSONObj createCursorResponse(CursorId cursorId,
                                            const BSONArray& docs);

        /**
         * Creates a listCollections response with given array of index specs.
         */
        static BSONObj createListCollectionsResponse(CursorId cursorId,
                                                     const BSONArray& colls,
                                                     const char* batchFieldName);

        static BSONObj createListCollectionsResponse(CursorId cursorId, const BSONArray& colls);

        /**
         * Creates a listIndexes response with given array of index specs.
         */
        static BSONObj createListIndexesResponse(CursorId cursorId,
                                                 const BSONArray& specs,
                                                 const char* batchFieldName);

        static BSONObj createListIndexesResponse(CursorId cursorId, const BSONArray& specs);

        static const HostAndPort target;
        static const NamespaceString nss;
        static const BSONObj idIndexSpec;

        BaseClonerTest();

        void setUp() override;
        void tearDown() override;

        virtual void clear();

        void setStatus(const Status& status);
        const Status& getStatus() const;
        void waitForStatus();

        void scheduleNetworkResponse(NetworkOperationIterator noi,
                                     const BSONObj& obj);
        void scheduleNetworkResponse(NetworkOperationIterator noi,
                                     ErrorCodes::Error code, const std::string& reason);
        void scheduleNetworkResponse(const BSONObj& obj);
        void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason);
        void processNetworkResponse(const BSONObj& obj);
        void processNetworkResponse(ErrorCodes::Error code, const std::string& reason);
        void finishProcessingNetworkResponse();

        /**
         * Tests life cycle functionality.
         */
        virtual BaseCloner* getCloner() const = 0;
        void testLifeCycle();

    private:

        // Protects member data of this base cloner fixture.
        mutable boost::mutex _mutex;

        boost::condition_variable _setStatusCondition;

        Status _status;

    };

    class StorageInterfaceMock : public CollectionCloner::StorageInterface {
    public:
        Status beginCollection(OperationContext* txn,
                               const NamespaceString& nss,
                               const CollectionOptions& options,
                               const std::vector<BSONObj>& specs) override;

        Status insertDocuments(OperationContext* txn,
                               const NamespaceString& nss,
                               const std::vector<BSONObj>& docs) override;

        stdx::function<Status (OperationContext*,
                               const NamespaceString&,
                               const CollectionOptions&,
                               const std::vector<BSONObj>&)> beginCollectionFn;

        stdx::function<Status (OperationContext*,
                               const NamespaceString&,
                               const std::vector<BSONObj>&)> insertDocumentsFn;
    };

} // namespace repl
} // namespace mongo
