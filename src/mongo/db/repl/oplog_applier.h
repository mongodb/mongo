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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {
namespace repl {

class SyncTail;

/**
 * Applies oplog entries.
 * Reads from an OplogBuffer batches of operations that may be applied in parallel.
 */
class OplogApplier {
public:
    /**
     * Used to configure behavior of this OplogApplier.
     **/
    class Options {
    public:
        bool allowNamespaceNotFoundErrorsOnCrudOps = false;
        bool relaxUniqueIndexConstraints = false;
        bool skipWritesToOplog = false;
    };

    /**
     * Controls what can popped from the oplog buffer into a single batch of operations that can be
     * applied using multiApply().
     */
    class BatchLimits {
    public:
        size_t bytes = 0;
        size_t ops = 0;

        // If provided, the batch will not include any operations with timestamps after this point.
        // This is intended for implementing slaveDelay, so it should be some number of seconds
        // before now.
        boost::optional<Date_t> slaveDelayLatestTimestamp = {};
    };

    // Used to report oplog application progress.
    class Observer;

    using Operations = std::vector<OplogEntry>;

    /**
     * Constructs this OplogApplier with specific options.
     * Obtains batches of operations from the OplogBuffer to apply.
     * Reports oplog application progress using the Observer.
     */
    OplogApplier(executor::TaskExecutor* executor,
                 OplogBuffer* oplogBuffer,
                 Observer* observer,
                 ReplicationCoordinator* replCoord,
                 ReplicationConsistencyMarkers* consistencyMarkers,
                 StorageInterface* storageInterface,
                 const Options& options,
                 ThreadPool* writerPool);

    /**
     * Starts this OplogApplier.
     * Use the Future object to be notified when this OplogApplier has finished shutting down.
     */
    Future<void> startup();

    /**
     * Starts the shutdown process for this OplogApplier.
     * It is safe to call shutdown() multiplie times.
     */
    void shutdown();

    /**
     * Pushes operations read into oplog buffer.
     */
    void enqueue(const Operations& operations);


private:
    // Used to schedule task for oplog application loop.
    // Not owned by us.
    executor::TaskExecutor* const _executor;

    // Not owned by us.
    OplogBuffer* const _oplogBuffer;

    // Not owned by us.
    Observer* const _observer;

    // Not owned by us.
    ReplicationCoordinator* const _replCoord;

    // Not owned by us.
    ReplicationConsistencyMarkers* const _consistencyMarkers;

    // Not owned by us.
    StorageInterface* const _storageInterface;

    // Used to configure OplogApplier behavior.
    const Options _options;

    // Used to run oplog application loop.
    std::unique_ptr<SyncTail> _syncTail;

    // Used to generate Future to allow callers to wait for oplog application shutdown.
    Promise<void> _promise;
};

/**
 * The OplogApplier reports its progress using the Observer interface.
 */
class OplogApplier::Observer {
public:
    virtual ~Observer() = default;

    /**
     * Called when the OplogApplier is ready to start applying a batch of operations read from the
     * OplogBuffer.
     **/
    virtual void onBatchBegin(const OplogApplier::Operations& operations) = 0;

    /**
     * When the OplogApplier has completed applying a batch of operations, it will call this
     * function to report the last optime applied on success. Any errors during oplog application
     * will also be here.
     */
    virtual void onBatchEnd(const StatusWith<OpTime>& lastOpTimeApplied,
                            const OplogApplier::Operations& operations) = 0;

    /**
     * Called when documents are fetched and inserted into the collection in order to
     * apply an update operation.
     * Applies to initial sync only.
     *
     * TODO: Delegate fetching behavior to OplogApplier owner.
     */
    using FetchInfo = std::pair<OplogEntry, BSONObj>;
    virtual void onMissingDocumentsFetchedAndInserted(
        const std::vector<FetchInfo>& documentsFetchedAndInserted) = 0;

    /**
     * Used primarily by BackgroundSync to update server statistics during steady state replication.
     * TODO: remove this function. See SERVER-33864.
     */
    virtual void onOperationConsumed(const BSONObj& op) = 0;
};

}  // namespace repl
}  // namespace mongo
