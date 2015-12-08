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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_delete_document.h"
#include "mongo/s/write_ops/batched_update_document.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class BSONObjBuilder;
class CurOp;
class LastError;
class OpCounters;
class OperationContext;
class WriteBatchStats;
struct WriteOpStats;

/**
 * An instance of WriteBatchExecutor is an object capable of issuing a write batch.
 */
class WriteBatchExecutor {
    MONGO_DISALLOW_COPYING(WriteBatchExecutor);

public:
    // State object used by private execInserts.  TODO: Do not expose this type.
    class ExecInsertsState;

    WriteBatchExecutor(OperationContext* txn, OpCounters* opCounters, LastError* le);

    /**
     * Issues writes with requested write concern.  Fills response with errors if problems
     * occur.
     */
    void executeBatch(const BatchedCommandRequest& request, BatchedCommandResponse* response);

    const WriteBatchStats& getStats() const;

    /**
     * Does basic validation of the batch request. Returns a non-OK status if
     * any problems with the batch are found.
     */
    static Status validateBatch(const BatchedCommandRequest& request);

private:
    /**
     * Executes the writes in the batch and returns upserted _ids and write errors.
     * Dispatches to one of the three functions below for DBLock, CurOp, and stats management.
     */
    void bulkExecute(const BatchedCommandRequest& request,
                     std::vector<BatchedUpsertDetail*>* upsertedIds,
                     std::vector<WriteErrorDetail*>* errors);

    /**
     * Inserts a subset of an insert batch.
     * Returns a true to discontinue the insert, or false if not.
     */
    bool insertMany(WriteBatchExecutor::ExecInsertsState* state,
                    size_t startIndex,
                    size_t endIndex,
                    CurOp* currentOp,
                    std::vector<WriteErrorDetail*>* errors,
                    bool ordered);

    /**
     * Executes the inserts of an insert batch and returns the write errors.
     *
     * Internally uses the DBLock of the request namespace.
     * May execute multiple inserts inside the same DBLock, and/or take the DBLock multiple
     * times.
     */
    void execInserts(const BatchedCommandRequest& request, std::vector<WriteErrorDetail*>* errors);

    /**
     * Executes an update item (which may update many documents or upsert), and returns the
     * upserted _id on upsert or error on failure.
     *
     * Internally uses the DBLock of the update namespace.
     * May take the DBLock multiple times.
     */
    void execUpdate(const BatchItemRef& updateItem, BSONObj* upsertedId, WriteErrorDetail** error);

    /**
     * Executes a delete item (which may remove many documents) and returns an error on failure.
     *
     * Internally uses the DBLock of the delete namespace.
     * May take the DBLock multiple times.
     */
    void execRemove(const BatchItemRef& removeItem, WriteErrorDetail** error);

    /**
     * Helper for incrementing stats after each individual write op.
     *
     * No lock requirements (though usually done inside write lock to make stats update look
     * atomic).
     */
    void incWriteStats(const BatchedCommandRequest::BatchType opType,
                       const WriteOpStats& stats,
                       const WriteErrorDetail* error,
                       CurOp* currentOp);

    OperationContext* _txn;

    // OpCounters object to update - needed for stats reporting
    // Not owned here.
    OpCounters* _opCounters;

    // LastError object to use for preparing write results - needed for stats reporting
    // Not owned here.
    LastError* _le;

    // Stats
    std::unique_ptr<WriteBatchStats> _stats;
};

/**
 * Holds information about the result of a single write operation.
 */
struct WriteOpStats {
    WriteOpStats() : n(0), nModified(0) {}

    void reset() {
        n = 0;
        nModified = 0;
        upsertedID = BSONObj();
    }

    // Num docs logically affected by this operation.
    int n;

    // Num docs actually modified by this operation, if applicable (update)
    int nModified;

    // _id of newly upserted document, if applicable (update)
    BSONObj upsertedID;
};

/**
 * Full stats accumulated by a write batch execution.  Note that these stats do not directly
 * correspond to the stats accumulated in opCounters and LastError.
 */
class WriteBatchStats {
public:
    WriteBatchStats()
        : numInserted(0), numUpserted(0), numMatched(0), numModified(0), numDeleted(0) {}

    int numInserted;
    int numUpserted;
    int numMatched;
    int numModified;
    int numDeleted;
};

}  // namespace mongo
