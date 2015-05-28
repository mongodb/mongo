/**
 *    Copyright (C) 2015 MongoDB Inc.
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
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/fetcher.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

    class CollectionCloner : public BaseCloner {
        MONGO_DISALLOW_COPYING(CollectionCloner);
    public:

        /**
         * Storage interface for collection cloner.
         *
         * Supports the operations on the storage layer required by the cloner.
         */
        class StorageInterface;

        /**
         * Type of function to schedule database work with the executor.
         *
         * Must be consistent with ReplicationExecutor::scheduleWorkWithGlobalExclusiveLock().
         *
         * Used for testing only.
         */
        using ScheduleDbWorkFn = stdx::function<StatusWith<ReplicationExecutor::CallbackHandle> (
            const ReplicationExecutor::CallbackFn&)>;

        /**
         * Creates CollectionCloner task in inactive state. Use start() to activate cloner.
         *
         * The cloner calls 'work' when the collection cloning has completed or failed.
         *
         * 'work' will be called exactly once.
         *
         * Takes ownership of the passed StorageInterface object.
         */
        CollectionCloner(ReplicationExecutor* executor,
                         const HostAndPort& source,
                         const NamespaceString& sourceNss,
                         const CollectionOptions& options,
                         const CallbackFn& work,
                         StorageInterface* storageInterface);

        virtual ~CollectionCloner() = default;

        const NamespaceString& getSourceNamespace() const;

        std::string getDiagnosticString() const override;

        bool isActive() const override;

        Status start() override;

        void cancel() override;

        //
        // Testing only functions below.
        //

        void wait() override;

        /**
         * Waits for database worker to complete.
         * Returns immediately if collection cloner is not active.
         *
         * For testing only.
         */
        void waitForDbWorker();

        /**
         * Overrides how executor schedules database work.
         *
         * For testing only.
         */
        void setScheduleDbWorkFn(const ScheduleDbWorkFn& scheduleDbWorkFn);

    private:

        /**
         * Read index specs from listIndexes result.
         */
        void _listIndexesCallback(const StatusWith<Fetcher::BatchData>& fetchResult,
                                  Fetcher::NextAction* nextAction,
                                  BSONObjBuilder* getMoreBob);

        /**
         * Read collection documents from find result.
         */
        void _findCallback(const StatusWith<Fetcher::BatchData>& fetchResult,
                           Fetcher::NextAction* nextAction,
                           BSONObjBuilder* getMoreBob);

        /**
         * Request storage interface to create collection.
         *
         * Called multiple times if there are more than one batch of responses from listIndexes
         * cursor.
         *
         * 'nextAction' is an in/out arg indicating the next action planned and to be taken
         *  by the fetcher.
         */
        void _beginCollectionCallback(const ReplicationExecutor::CallbackData& callbackData);

        /**
         * Called multiple times if there are more than one batch of documents from the fetcher.
         * On the last batch, 'lastBatch' will be true.
         *
         * Each document returned will be inserted via the storage interfaceRequest storage
         * interface.
         */
        void _insertDocumentsCallback(const ReplicationExecutor::CallbackData& callbackData,
                                      bool lastBatch);

        // Not owned by us.
        ReplicationExecutor* _executor;

        HostAndPort _source;
        NamespaceString _sourceNss;
        NamespaceString _destNss;
        CollectionOptions _options;

        // Invoked once when cloning completes or fails.
        CallbackFn _work;

        // Owned by us.
        std::unique_ptr<StorageInterface> _storageInterface;

        // Protects member data of this collection cloner.
        mutable boost::mutex _mutex;

        // _active is true when Collection Cloner is started.
        bool _active;

        // Fetcher instances for running listIndexes and find commands.
        Fetcher _listIndexesFetcher;
        Fetcher _findFetcher;

        std::vector<BSONObj> _indexSpecs;

        // Current batch of documents read from fetcher to insert into collection.
        std::vector<BSONObj> _documents;

        // Callback handle for database worker.
        ReplicationExecutor::CallbackHandle _dbWorkCallbackHandle;

        // Function for scheduling database work using the executor.
        ScheduleDbWorkFn _scheduleDbWorkFn;

    };

    /**
     * Storage interface used by the collection cloner to build a collection.
     *
     * Operation context is provided by the replication executor via the cloner.
     *
     * The storage interface is expected to acquire locks on any resources it needs
     * to perform any of its functions.
     *
     * TODO: Consider having commit/abort/cancel functions.
     */
    class CollectionCloner::StorageInterface {
    public:

        /**
         * When the storage interface is destroyed, it will commit the index builder.
         */
        virtual ~StorageInterface() = default;

        /**
         * Creates a collection with the provided indexes.
         *
         * Assume that no database locks have been acquired prior to calling this
         * function.
         */
        virtual Status beginCollection(OperationContext* txn,
                                       const NamespaceString& nss,
                                       const CollectionOptions& options,
                                       const std::vector<BSONObj>& indexSpecs) = 0;

        /**
         * Inserts documents into a collection.
         *
         * Assume that no database locks have been acquired prior to calling this
         * function.
         */
        virtual Status insertDocuments(OperationContext* txn,
                                       const NamespaceString& nss,
                                       const std::vector<BSONObj>& documents) = 0;

    };

} // namespace repl
} // namespace mongo
