/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batch_write_exec.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/s/client/multi_command_dispatch.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/util/log.h"

namespace mongo {

using std::make_pair;
using std::stringstream;
using std::vector;

BatchWriteExec::BatchWriteExec(NSTargeter* targeter, MultiCommandDispatch* dispatcher)
    : _targeter(targeter), _dispatcher(dispatcher) {}

namespace {

//
// Map which allows associating ConnectionString hosts with TargetedWriteBatches
// This is needed since the dispatcher only returns hosts with responses.
//

// TODO: Unordered map?
typedef OwnedPointerMap<ConnectionString, TargetedWriteBatch> OwnedHostBatchMap;
}

static void buildErrorFrom(const Status& status, WriteErrorDetail* error) {
    error->setErrCode(status.code());
    error->setErrMessage(status.reason());
}

// Helper to note several stale errors from a response
static void noteStaleResponses(const vector<ShardError*>& staleErrors, NSTargeter* targeter) {
    for (vector<ShardError*>::const_iterator it = staleErrors.begin(); it != staleErrors.end();
         ++it) {
        const ShardError* error = *it;
        targeter->noteStaleResponse(
            error->endpoint, error->error.isErrInfoSet() ? error->error.getErrInfo() : BSONObj());
    }
}

// The number of times we'll try to continue a batch op if no progress is being made
// This only applies when no writes are occurring and metadata is not changing on reload
static const int kMaxRoundsWithoutProgress(5);

void BatchWriteExec::executeBatch(OperationContext* txn,
                                  const BatchedCommandRequest& clientRequest,
                                  BatchedCommandResponse* clientResponse,
                                  BatchWriteExecStats* stats) {
    LOG(4) << "starting execution of write batch of size "
           << static_cast<int>(clientRequest.sizeWriteOps()) << " for " << clientRequest.getNS();

    BatchWriteOp batchOp;
    batchOp.initClientRequest(&clientRequest);

    // Current batch status
    bool refreshedTargeter = false;
    int rounds = 0;
    int numCompletedOps = 0;
    int numRoundsWithoutProgress = 0;

    while (!batchOp.isFinished()) {
        //
        // Get child batches to send using the targeter
        //
        // Targeting errors can be caused by remote metadata changing (the collection could have
        // been dropped and recreated, for example with a new shard key).  If a remote metadata
        // change occurs *before* a client sends us a batch, we need to make sure that we don't
        // error out just because we're staler than the client - otherwise mongos will be have
        // unpredictable behavior.
        //
        // (If a metadata change happens *during* or *after* a client sends us a batch, however,
        // we make no guarantees about delivery.)
        //
        // For this reason, we don't record targeting errors until we've refreshed our targeting
        // metadata at least once *after* receiving the client batch - at that point, we know:
        //
        // 1) our new metadata is the same as the metadata when the client sent a batch, and so
        //    targeting errors are real.
        // OR
        // 2) our new metadata is a newer version than when the client sent a batch, and so
        //    the metadata must have changed after the client batch was sent.  We don't need to
        //    deliver in this case, since for all the client knows we may have gotten the batch
        //    exactly when the metadata changed.
        //

        OwnedPointerVector<TargetedWriteBatch> childBatchesOwned;
        vector<TargetedWriteBatch*>& childBatches = childBatchesOwned.mutableVector();

        // If we've already had a targeting error, we've refreshed the metadata once and can
        // record target errors definitively.
        bool recordTargetErrors = refreshedTargeter;
        Status targetStatus =
            batchOp.targetBatch(txn, *_targeter, recordTargetErrors, &childBatches);
        if (!targetStatus.isOK()) {
            // Don't do anything until a targeter refresh
            _targeter->noteCouldNotTarget();
            refreshedTargeter = true;
            ++stats->numTargetErrors;
            dassert(childBatches.size() == 0u);
        }

        //
        // Send all child batches
        //

        size_t numSent = 0;
        size_t numToSend = childBatches.size();
        while (numSent != numToSend) {
            // Collect batches out on the network, mapped by endpoint
            OwnedHostBatchMap ownedPendingBatches;
            OwnedHostBatchMap::MapType& pendingBatches = ownedPendingBatches.mutableMap();

            //
            // Send side
            //

            // Get as many batches as we can at once
            for (vector<TargetedWriteBatch*>::iterator it = childBatches.begin();
                 it != childBatches.end();
                 ++it) {
                //
                // Collect the info needed to dispatch our targeted batch
                //

                TargetedWriteBatch* nextBatch = *it;
                // If the batch is NULL, we sent it previously, so skip
                if (nextBatch == NULL)
                    continue;

                // Figure out what host we need to dispatch our targeted batch
                bool resolvedHost = true;
                const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet());
                auto shard =
                    grid.shardRegistry()->getShard(txn, nextBatch->getEndpoint().shardName);

                if (!shard) {
                    Status status = Status(ErrorCodes::ShardNotFound,
                                           str::stream() << "unknown shard name "
                                                         << nextBatch->getEndpoint().shardName);
                    resolvedHost = false;

                    // Record a resolve failure
                    // TODO: It may be necessary to refresh the cache if stale, or maybe just
                    // cancel and retarget the batch
                    WriteErrorDetail error;
                    buildErrorFrom(status, &error);
                    LOG(4) << "unable to send write batch to " << nextBatch->getEndpoint().shardName
                           << causedBy(status);
                    batchOp.noteBatchError(*nextBatch, error);
                }

                auto swHostAndPort = shard->getTargeter()->findHost(readPref);
                if (!swHostAndPort.isOK()) {
                    resolvedHost = false;

                    // Record a resolve failure
                    // TODO: It may be necessary to refresh the cache if stale, or maybe just
                    // cancel and retarget the batch
                    WriteErrorDetail error;
                    buildErrorFrom(swHostAndPort.getStatus(), &error);
                    LOG(4) << "unable to send write batch to " << nextBatch->getEndpoint().shardName
                           << causedBy(swHostAndPort.getStatus());
                    batchOp.noteBatchError(*nextBatch, error);
                }

                if (!resolvedHost) {
                    ++stats->numResolveErrors;

                    // We're done with this batch
                    // Clean up when we can't resolve a host
                    delete *it;
                    *it = NULL;
                    --numToSend;
                    continue;
                }

                ConnectionString shardHost(swHostAndPort.getValue());

                // If we already have a batch for this host, wait until the next time
                OwnedHostBatchMap::MapType::iterator pendingIt = pendingBatches.find(shardHost);
                if (pendingIt != pendingBatches.end())
                    continue;

                //
                // We now have all the info needed to dispatch the batch
                //

                BatchedCommandRequest request(clientRequest.getBatchType());
                batchOp.buildBatchRequest(*nextBatch, &request);

                // Internally we use full namespaces for request/response, but we send the
                // command to a database with the collection name in the request.
                NamespaceString nss(request.getNS());
                request.setNS(nss);

                LOG(4) << "sending write batch to " << shardHost.toString() << ": "
                       << request.toString();

                _dispatcher->addCommand(shardHost, nss.db(), request.toBSON());

                // Indicate we're done by setting the batch to NULL
                // We'll only get duplicate hostEndpoints if we have broadcast and non-broadcast
                // endpoints for the same host, so this should be pretty efficient without
                // moving stuff around.
                *it = NULL;

                // Recv-side is responsible for cleaning up the nextBatch when used
                pendingBatches.insert(make_pair(shardHost, nextBatch));
            }

            // Send them all out
            _dispatcher->sendAll();
            numSent += pendingBatches.size();

            //
            // Recv side
            //

            while (_dispatcher->numPending() > 0) {
                // Get the response
                ConnectionString shardHost;
                BatchedCommandResponse response;
                Status dispatchStatus = _dispatcher->recvAny(&shardHost, &response);

                // Get the TargetedWriteBatch to find where to put the response
                dassert(pendingBatches.find(shardHost) != pendingBatches.end());
                TargetedWriteBatch* batch = pendingBatches.find(shardHost)->second;

                if (dispatchStatus.isOK()) {
                    TrackedErrors trackedErrors;
                    trackedErrors.startTracking(ErrorCodes::StaleShardVersion);

                    LOG(4) << "write results received from " << shardHost.toString() << ": "
                           << response.toString();

                    // Dispatch was ok, note response
                    batchOp.noteBatchResponse(*batch, response, &trackedErrors);

                    // Note if anything was stale
                    const vector<ShardError*>& staleErrors =
                        trackedErrors.getErrors(ErrorCodes::StaleShardVersion);

                    if (staleErrors.size() > 0) {
                        noteStaleResponses(staleErrors, _targeter);
                        ++stats->numStaleBatches;
                    }

                    // Remember that we successfully wrote to this shard
                    // NOTE: This will record lastOps for shards where we actually didn't update
                    // or delete any documents, which preserves old behavior but is conservative
                    stats->noteWriteAt(
                        shardHost,
                        response.isLastOpSet() ? response.getLastOp() : repl::OpTime(),
                        response.isElectionIdSet() ? response.getElectionId() : OID());
                } else {
                    // Error occurred dispatching, note it

                    stringstream msg;
                    msg << "write results unavailable from " << shardHost.toString()
                        << causedBy(dispatchStatus.toString());

                    WriteErrorDetail error;
                    buildErrorFrom(Status(ErrorCodes::RemoteResultsUnavailable, msg.str()), &error);

                    LOG(4) << "unable to receive write results from " << shardHost.toString()
                           << causedBy(dispatchStatus.toString());

                    batchOp.noteBatchError(*batch, error);
                }
            }
        }

        ++rounds;
        ++stats->numRounds;

        // If we're done, get out
        if (batchOp.isFinished())
            break;

        // MORE WORK TO DO

        //
        // Refresh the targeter if we need to (no-op if nothing stale)
        //

        bool targeterChanged = false;
        Status refreshStatus = _targeter->refreshIfNeeded(txn, &targeterChanged);

        if (!refreshStatus.isOK()) {
            // It's okay if we can't refresh, we'll just record errors for the ops if
            // needed.
            warning() << "could not refresh targeter" << causedBy(refreshStatus.reason());
        }

        //
        // Ensure progress is being made toward completing the batch op
        //

        int currCompletedOps = batchOp.numWriteOpsIn(WriteOpState_Completed);
        if (currCompletedOps == numCompletedOps && !targeterChanged) {
            ++numRoundsWithoutProgress;
        } else {
            numRoundsWithoutProgress = 0;
        }
        numCompletedOps = currCompletedOps;

        if (numRoundsWithoutProgress > kMaxRoundsWithoutProgress) {
            stringstream msg;
            msg << "no progress was made executing batch write op in " << clientRequest.getNS().ns()
                << " after " << kMaxRoundsWithoutProgress << " rounds (" << numCompletedOps
                << " ops completed in " << rounds << " rounds total)";

            WriteErrorDetail error;
            buildErrorFrom(Status(ErrorCodes::NoProgressMade, msg.str()), &error);
            batchOp.abortBatch(error);
            break;
        }
    }

    batchOp.buildClientResponse(clientResponse);

    LOG(4) << "finished execution of write batch"
           << (clientResponse->isErrDetailsSet() ? " with write errors" : "")
           << (clientResponse->isErrDetailsSet() && clientResponse->isWriteConcernErrorSet()
                   ? " and"
                   : "")
           << (clientResponse->isWriteConcernErrorSet() ? " with write concern error" : "")
           << " for " << clientRequest.getNS();
}

void BatchWriteExecStats::noteWriteAt(const ConnectionString& host,
                                      repl::OpTime opTime,
                                      const OID& electionId) {
    _writeOpTimes[host] = HostOpTime(opTime, electionId);
}

const HostOpTimeMap& BatchWriteExecStats::getWriteOpTimes() const {
    return _writeOpTimes;
}
}
