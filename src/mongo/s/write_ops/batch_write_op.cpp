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

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batch_write_op.h"

#include "mongo/base/error_codes.h"

namespace mongo {

using std::unique_ptr;
using std::make_pair;
using std::set;
using std::stringstream;
using std::vector;

/**
 * Returns a new write concern that has the copy of every field from the original
 * document but with a w set to 1. This is intended for upgrading { w: 0 } write
 * concern to { w: 1 }.
 */
static BSONObj upgradeWriteConcern(const BSONObj& origWriteConcern) {
    BSONObjIterator iter(origWriteConcern);
    BSONObjBuilder newWriteConcern;

    while (iter.more()) {
        BSONElement elem(iter.next());

        if (strncmp(elem.fieldName(), "w", 2) == 0) {
            newWriteConcern.append("w", 1);
        } else {
            newWriteConcern.append(elem);
        }
    }

    return newWriteConcern.obj();
}

BatchWriteStats::BatchWriteStats()
    : numInserted(0), numUpserted(0), numMatched(0), numModified(0), numDeleted(0) {}

BatchWriteOp::BatchWriteOp() : _clientRequest(NULL), _writeOps(NULL), _stats(new BatchWriteStats) {}

void BatchWriteOp::initClientRequest(const BatchedCommandRequest* clientRequest) {
    dassert(clientRequest->isValid(NULL));

    size_t numWriteOps = clientRequest->sizeWriteOps();
    _writeOps = static_cast<WriteOp*>(::operator new[](numWriteOps * sizeof(WriteOp)));

    for (size_t i = 0; i < numWriteOps; ++i) {
        // Don't want to have to define what an empty WriteOp means, so construct in-place
        new (&_writeOps[i]) WriteOp(BatchItemRef(clientRequest, i));
    }

    _clientRequest = clientRequest;
}

// Arbitrary endpoint ordering, needed for grouping by endpoint
static int compareEndpoints(const ShardEndpoint* endpointA, const ShardEndpoint* endpointB) {
    int shardNameDiff = endpointA->shardName.compare(endpointB->shardName);
    if (shardNameDiff != 0)
        return shardNameDiff;

    long shardVersionDiff = endpointA->shardVersion.toLong() - endpointB->shardVersion.toLong();
    if (shardVersionDiff != 0)
        return shardVersionDiff;

    int shardEpochDiff = endpointA->shardVersion.epoch().compare(endpointB->shardVersion.epoch());
    return shardEpochDiff;
}

namespace {

//
// Types for comparing shard endpoints in a map
//

struct EndpointComp {
    bool operator()(const ShardEndpoint* endpointA, const ShardEndpoint* endpointB) const {
        return compareEndpoints(endpointA, endpointB) < 0;
    }
};

typedef std::map<const ShardEndpoint*, TargetedWriteBatch*, EndpointComp> TargetedBatchMap;

//
// Types for tracking batch sizes
//

struct BatchSize {
    BatchSize() : numOps(0), sizeBytes(0) {}

    int numOps;
    int sizeBytes;
};

typedef std::map<const ShardEndpoint*, BatchSize, EndpointComp> TargetedBatchSizeMap;
}

static void buildTargetError(const Status& errStatus, WriteErrorDetail* details) {
    details->setErrCode(errStatus.code());
    details->setErrMessage(errStatus.reason());
}

// Helper to determine whether a number of targeted writes require a new targeted batch
static bool isNewBatchRequired(const vector<TargetedWrite*>& writes,
                               const TargetedBatchMap& batchMap) {
    for (vector<TargetedWrite*>::const_iterator it = writes.begin(); it != writes.end(); ++it) {
        TargetedWrite* write = *it;
        if (batchMap.find(&write->endpoint) == batchMap.end()) {
            return true;
        }
    }

    return false;
}

// MAGIC NUMBERS
// Before serializing updates/deletes, we don't know how big their fields would be, but we break
// batches before serializing.
// TODO: Revisit when we revisit command limits in general
static const int kEstUpdateOverheadBytes = (BSONObjMaxInternalSize - BSONObjMaxUserSize) / 100;
static const int kEstDeleteOverheadBytes = (BSONObjMaxInternalSize - BSONObjMaxUserSize) / 100;

static int getWriteSizeBytes(const WriteOp& writeOp) {
    const BatchItemRef& item = writeOp.getWriteItem();
    BatchedCommandRequest::BatchType batchType = item.getOpType();

    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        return item.getDocument().objsize();
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        // Note: Be conservative here - it's okay if we send slightly too many batches
        int estSize = item.getUpdate()->getQuery().objsize() +
            item.getUpdate()->getUpdateExpr().objsize() + kEstUpdateOverheadBytes;
        dassert(estSize >= item.getUpdate()->toBSON().objsize());
        return estSize;
    } else {
        dassert(batchType == BatchedCommandRequest::BatchType_Delete);
        // Note: Be conservative here - it's okay if we send slightly too many batches
        int estSize = item.getDelete()->getQuery().objsize() + kEstDeleteOverheadBytes;
        dassert(estSize >= item.getDelete()->toBSON().objsize());
        return estSize;
    }
}

// Helper to determine whether a number of targeted writes require a new targeted batch
static bool wouldMakeBatchesTooBig(const vector<TargetedWrite*>& writes,
                                   int writeSizeBytes,
                                   const TargetedBatchSizeMap& batchSizes) {
    for (vector<TargetedWrite*>::const_iterator it = writes.begin(); it != writes.end(); ++it) {
        const TargetedWrite* write = *it;
        TargetedBatchSizeMap::const_iterator seenIt = batchSizes.find(&write->endpoint);

        if (seenIt == batchSizes.end()) {
            // If this is the first item in the batch, it can't be too big
            continue;
        }

        const BatchSize& batchSize = seenIt->second;

        if (batchSize.numOps >= static_cast<int>(BatchedCommandRequest::kMaxWriteBatchSize)) {
            // Too many items in batch
            return true;
        }

        if (batchSize.sizeBytes + writeSizeBytes > BSONObjMaxUserSize) {
            // Batch would be too big
            return true;
        }
    }

    return false;
}

// Helper function to cancel all the write ops of targeted batches in a map
static void cancelBatches(const WriteErrorDetail& why,
                          WriteOp* writeOps,
                          TargetedBatchMap* batchMap) {
    set<WriteOp*> targetedWriteOps;

    // Collect all the writeOps that are currently targeted
    for (TargetedBatchMap::iterator it = batchMap->begin(); it != batchMap->end();) {
        TargetedWriteBatch* batch = it->second;
        const vector<TargetedWrite*>& writes = batch->getWrites();

        for (vector<TargetedWrite*>::const_iterator writeIt = writes.begin();
             writeIt != writes.end();
             ++writeIt) {
            TargetedWrite* write = *writeIt;

            // NOTE: We may repeatedly cancel a write op here, but that's fast and we want to
            // cancel before erasing the TargetedWrite* (which owns the cancelled targeting
            // info) for reporting reasons.
            writeOps[write->writeOpRef.first].cancelWrites(&why);
        }

        // Note that we need to *erase* first, *then* delete, since the map keys are ptrs from
        // the values
        batchMap->erase(it++);
        delete batch;
    }
    batchMap->clear();
}

Status BatchWriteOp::targetBatch(OperationContext* txn,
                                 const NSTargeter& targeter,
                                 bool recordTargetErrors,
                                 vector<TargetedWriteBatch*>* targetedBatches) {
    //
    // Targeting of unordered batches is fairly simple - each remaining write op is targeted,
    // and each of those targeted writes are grouped into a batch for a particular shard
    // endpoint.
    //
    // Targeting of ordered batches is a bit more complex - to respect the ordering of the
    // batch, we can only send:
    // A) a single targeted batch to one shard endpoint
    // B) multiple targeted batches, but only containing targeted writes for a single write op
    //
    // This means that any multi-shard write operation must be targeted and sent one-by-one.
    // Subsequent single-shard write operations can be batched together if they go to the same
    // place.
    //
    // Ex: ShardA : { skey : a->k }, ShardB : { skey : k->z }
    //
    // Ordered insert batch of: [{ skey : a }, { skey : b }, { skey : x }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : x }]
    //
    // Ordered update Batch of :
    //  [{ skey : a }{ $push },
    //   { skey : b }{ $push },
    //   { skey : [c, x] }{ $push },
    //   { skey : y }{ $push },
    //   { skey : z }{ $push }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : [c,x] }],
    //  [{ skey : y }, { skey : z }]
    //

    const bool ordered = _clientRequest->getOrdered();

    TargetedBatchMap batchMap;
    TargetedBatchSizeMap batchSizes;

    int numTargetErrors = 0;

    size_t numWriteOps = _clientRequest->sizeWriteOps();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];

        // Only target _Ready ops
        if (writeOp.getWriteState() != WriteOpState_Ready)
            continue;

        //
        // Get TargetedWrites from the targeter for the write operation
        //

        // TargetedWrites need to be owned once returned
        OwnedPointerVector<TargetedWrite> writesOwned;
        vector<TargetedWrite*>& writes = writesOwned.mutableVector();

        Status targetStatus = writeOp.targetWrites(txn, targeter, &writes);

        if (!targetStatus.isOK()) {
            WriteErrorDetail targetError;
            buildTargetError(targetStatus, &targetError);

            if (!recordTargetErrors) {
                // Cancel current batch state with an error

                cancelBatches(targetError, _writeOps, &batchMap);
                dassert(batchMap.empty());
                return targetStatus;
            } else if (!ordered || batchMap.empty()) {
                // Record an error for this batch

                writeOp.setOpError(targetError);
                ++numTargetErrors;

                if (ordered)
                    return Status::OK();

                continue;
            } else {
                dassert(ordered && !batchMap.empty());

                // Send out what we have, but don't record an error yet, since there may be an
                // error in the writes before this point.

                writeOp.cancelWrites(&targetError);
                break;
            }
        }

        //
        // If ordered and we have a previous endpoint, make sure we don't need to send these
        // targeted writes to any other endpoints.
        //

        if (ordered && !batchMap.empty()) {
            dassert(batchMap.size() == 1u);
            if (isNewBatchRequired(writes, batchMap)) {
                writeOp.cancelWrites(NULL);
                break;
            }
        }

        //
        // If this write will push us over some sort of size limit, stop targeting
        //

        int writeSizeBytes = getWriteSizeBytes(writeOp);
        if (wouldMakeBatchesTooBig(writes, writeSizeBytes, batchSizes)) {
            invariant(!batchMap.empty());
            writeOp.cancelWrites(NULL);
            break;
        }

        //
        // Targeting went ok, add to appropriate TargetedBatch
        //

        for (vector<TargetedWrite*>::iterator it = writes.begin(); it != writes.end(); ++it) {
            TargetedWrite* write = *it;

            TargetedBatchMap::iterator batchIt = batchMap.find(&write->endpoint);
            TargetedBatchSizeMap::iterator batchSizeIt = batchSizes.find(&write->endpoint);

            if (batchIt == batchMap.end()) {
                TargetedWriteBatch* newBatch = new TargetedWriteBatch(write->endpoint);
                batchIt = batchMap.insert(make_pair(&newBatch->getEndpoint(), newBatch)).first;
                batchSizeIt =
                    batchSizes.insert(make_pair(&newBatch->getEndpoint(), BatchSize())).first;
            }

            TargetedWriteBatch* batch = batchIt->second;
            BatchSize& batchSize = batchSizeIt->second;

            ++batchSize.numOps;
            batchSize.sizeBytes += writeSizeBytes;
            batch->addWrite(write);
        }

        // Relinquish ownership of TargetedWrites, now the TargetedBatches own them
        writesOwned.mutableVector().clear();

        //
        // Break if we're ordered and we have more than one endpoint - later writes cannot be
        // enforced as ordered across multiple shard endpoints.
        //

        if (ordered && batchMap.size() > 1u)
            break;
    }

    //
    // Send back our targeted batches
    //

    for (TargetedBatchMap::iterator it = batchMap.begin(); it != batchMap.end(); ++it) {
        TargetedWriteBatch* batch = it->second;

        if (batch->getWrites().empty())
            continue;

        // Remember targeted batch for reporting
        _targeted.insert(batch);
        // Send the handle back to caller
        targetedBatches->push_back(batch);
    }

    return Status::OK();
}

void BatchWriteOp::buildBatchRequest(const TargetedWriteBatch& targetedBatch,
                                     BatchedCommandRequest* request) const {
    request->setNS(_clientRequest->getNS());
    request->setShouldBypassValidation(_clientRequest->shouldBypassValidation());

    const vector<TargetedWrite*>& targetedWrites = targetedBatch.getWrites();

    for (vector<TargetedWrite*>::const_iterator it = targetedWrites.begin();
         it != targetedWrites.end();
         ++it) {
        const WriteOpRef& writeOpRef = (*it)->writeOpRef;
        BatchedCommandRequest::BatchType batchType = _clientRequest->getBatchType();

        // NOTE:  We copy the batch items themselves here from the client request
        // TODO: This could be inefficient, maybe we want to just reference in the future
        if (batchType == BatchedCommandRequest::BatchType_Insert) {
            BatchedInsertRequest* clientInsertRequest = _clientRequest->getInsertRequest();
            BSONObj insertDoc = clientInsertRequest->getDocumentsAt(writeOpRef.first);
            request->getInsertRequest()->addToDocuments(insertDoc);
        } else if (batchType == BatchedCommandRequest::BatchType_Update) {
            BatchedUpdateRequest* clientUpdateRequest = _clientRequest->getUpdateRequest();
            BatchedUpdateDocument* updateDoc = new BatchedUpdateDocument;
            clientUpdateRequest->getUpdatesAt(writeOpRef.first)->cloneTo(updateDoc);
            request->getUpdateRequest()->addToUpdates(updateDoc);
        } else {
            dassert(batchType == BatchedCommandRequest::BatchType_Delete);
            BatchedDeleteRequest* clientDeleteRequest = _clientRequest->getDeleteRequest();
            BatchedDeleteDocument* deleteDoc = new BatchedDeleteDocument;
            clientDeleteRequest->getDeletesAt(writeOpRef.first)->cloneTo(deleteDoc);
            request->getDeleteRequest()->addToDeletes(deleteDoc);
        }

        // TODO: We can add logic here to allow aborting individual ops
        // if ( NULL == response ) {
        //    ->responses.erase( it++ );
        //    continue;
        //}
    }

    if (_clientRequest->isWriteConcernSet()) {
        if (_clientRequest->isVerboseWC()) {
            request->setWriteConcern(_clientRequest->getWriteConcern());
        } else {
            // Mongos needs to send to the shard with w > 0 so it will be able to
            // see the writeErrors.
            request->setWriteConcern(upgradeWriteConcern(_clientRequest->getWriteConcern()));
        }
    }

    if (!request->isOrderedSet()) {
        request->setOrdered(_clientRequest->getOrdered());
    }

    request->setShardVersion(targetedBatch.getEndpoint().shardVersion);
}

//
// Helpers for manipulating batch responses
//

namespace {
struct WriteErrorDetailComp {
    bool operator()(const WriteErrorDetail* errorA, const WriteErrorDetail* errorB) const {
        return errorA->getIndex() < errorB->getIndex();
    }
};
}

static void cloneCommandErrorTo(const BatchedCommandResponse& batchResp,
                                WriteErrorDetail* details) {
    details->setErrCode(batchResp.getErrCode());
    details->setErrMessage(batchResp.getErrMessage());
}

// Given *either* a batch error or an array of per-item errors, copies errors we're interested
// in into a TrackedErrorMap
static void trackErrors(const ShardEndpoint& endpoint,
                        const vector<WriteErrorDetail*> itemErrors,
                        TrackedErrors* trackedErrors) {
    for (vector<WriteErrorDetail*>::const_iterator it = itemErrors.begin(); it != itemErrors.end();
         ++it) {
        const WriteErrorDetail* error = *it;

        if (trackedErrors->isTracking(error->getErrCode())) {
            trackedErrors->addError(new ShardError(endpoint, *error));
        }
    }
}

static void incBatchStats(BatchedCommandRequest::BatchType batchType,
                          const BatchedCommandResponse& response,
                          BatchWriteStats* stats) {
    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        stats->numInserted += response.getN();
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        int numUpserted = 0;
        if (response.isUpsertDetailsSet()) {
            numUpserted = response.sizeUpsertDetails();
        }
        stats->numMatched += (response.getN() - numUpserted);
        long long numModified = response.getNModified();

        if (numModified >= 0)
            stats->numModified += numModified;
        else
            stats->numModified = -1;  // sentinel used to indicate we omit the field downstream

        stats->numUpserted += numUpserted;
    } else {
        dassert(batchType == BatchedCommandRequest::BatchType_Delete);
        stats->numDeleted += response.getN();
    }
}

void BatchWriteOp::noteBatchResponse(const TargetedWriteBatch& targetedBatch,
                                     const BatchedCommandResponse& response,
                                     TrackedErrors* trackedErrors) {
    if (!response.getOk()) {
        WriteErrorDetail error;
        cloneCommandErrorTo(response, &error);

        // Treat command errors exactly like other failures of the batch
        // Note that no errors will be tracked from these failures - as-designed
        noteBatchError(targetedBatch, error);
        return;
    }

    dassert(response.getOk());

    // Stop tracking targeted batch
    _targeted.erase(&targetedBatch);

    // Increment stats for this batch
    incBatchStats(_clientRequest->getBatchType(), response, _stats.get());

    //
    // Assign errors to particular items.
    // Write Concern errors are stored and handled later.
    //

    // Special handling for write concern errors, save for later
    if (response.isWriteConcernErrorSet()) {
        unique_ptr<ShardWCError> wcError(
            new ShardWCError(targetedBatch.getEndpoint(), *response.getWriteConcernError()));
        _wcErrors.mutableVector().push_back(wcError.release());
    }

    vector<WriteErrorDetail*> itemErrors;

    // Handle batch and per-item errors
    if (response.isErrDetailsSet()) {
        // Per-item errors were set
        itemErrors.insert(
            itemErrors.begin(), response.getErrDetails().begin(), response.getErrDetails().end());

        // Sort per-item errors by index
        std::sort(itemErrors.begin(), itemErrors.end(), WriteErrorDetailComp());
    }

    //
    // Go through all pending responses of the op and sorted remote reponses, populate errors
    // This will either set all errors to the batch error or apply per-item errors as-needed
    //
    // If the batch is ordered, cancel all writes after the first error for retargeting.
    //

    bool ordered = _clientRequest->getOrdered();

    vector<WriteErrorDetail*>::iterator itemErrorIt = itemErrors.begin();
    int index = 0;
    WriteErrorDetail* lastError = NULL;
    for (vector<TargetedWrite *>::const_iterator it = targetedBatch.getWrites().begin();
         it != targetedBatch.getWrites().end();
         ++it, ++index) {
        const TargetedWrite* write = *it;
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];

        dassert(writeOp.getWriteState() == WriteOpState_Pending);

        // See if we have an error for the write
        WriteErrorDetail* writeError = NULL;

        if (itemErrorIt != itemErrors.end() && (*itemErrorIt)->getIndex() == index) {
            // We have an per-item error for this write op's index
            writeError = *itemErrorIt;
            ++itemErrorIt;
        }

        // Finish the response (with error, if needed)
        if (NULL == writeError) {
            if (!ordered || !lastError) {
                writeOp.noteWriteComplete(*write);
            } else {
                // We didn't actually apply this write - cancel so we can retarget
                dassert(writeOp.getNumTargeted() == 1u);
                writeOp.cancelWrites(lastError);
            }
        } else {
            writeOp.noteWriteError(*write, *writeError);
            lastError = writeError;
        }
    }

    // Track errors we care about, whether batch or individual errors
    if (NULL != trackedErrors) {
        trackErrors(targetedBatch.getEndpoint(), itemErrors, trackedErrors);
    }

    // Track upserted ids if we need to
    if (response.isUpsertDetailsSet()) {
        const vector<BatchedUpsertDetail*>& upsertedIds = response.getUpsertDetails();
        for (vector<BatchedUpsertDetail*>::const_iterator it = upsertedIds.begin();
             it != upsertedIds.end();
             ++it) {
            // The child upserted details don't have the correct index for the full batch
            const BatchedUpsertDetail* childUpsertedId = *it;

            // Work backward from the child batch item index to the batch item index
            int childBatchIndex = childUpsertedId->getIndex();
            int batchIndex = targetedBatch.getWrites()[childBatchIndex]->writeOpRef.first;

            // Push the upserted id with the correct index into the batch upserted ids
            BatchedUpsertDetail* upsertedId = new BatchedUpsertDetail;
            upsertedId->setIndex(batchIndex);
            upsertedId->setUpsertedID(childUpsertedId->getUpsertedID());
            _upsertedIds.mutableVector().push_back(upsertedId);
        }
    }
}

static void toWriteErrorResponse(const WriteErrorDetail& error,
                                 bool ordered,
                                 int numWrites,
                                 BatchedCommandResponse* writeErrResponse) {
    writeErrResponse->setOk(true);
    writeErrResponse->setN(0);

    int numErrors = ordered ? 1 : numWrites;
    for (int i = 0; i < numErrors; i++) {
        unique_ptr<WriteErrorDetail> errorClone(new WriteErrorDetail);
        error.cloneTo(errorClone.get());
        errorClone->setIndex(i);
        writeErrResponse->addToErrDetails(errorClone.release());
    }

    dassert(writeErrResponse->isValid(NULL));
}

void BatchWriteOp::noteBatchError(const TargetedWriteBatch& targetedBatch,
                                  const WriteErrorDetail& error) {
    // Treat errors to get a batch response as failures of the contained writes
    BatchedCommandResponse emulatedResponse;
    toWriteErrorResponse(
        error, _clientRequest->getOrdered(), targetedBatch.getWrites().size(), &emulatedResponse);

    noteBatchResponse(targetedBatch, emulatedResponse, NULL);
}

void BatchWriteOp::abortBatch(const WriteErrorDetail& error) {
    dassert(!isFinished());
    dassert(numWriteOpsIn(WriteOpState_Pending) == 0);

    size_t numWriteOps = _clientRequest->sizeWriteOps();
    bool orderedOps = _clientRequest->getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        // Can only be called with no outstanding batches
        dassert(writeOp.getWriteState() != WriteOpState_Pending);

        if (writeOp.getWriteState() < WriteOpState_Completed) {
            writeOp.setOpError(error);

            // Only one error if we're ordered
            if (orderedOps)
                break;
        }
    }

    dassert(isFinished());
}

bool BatchWriteOp::isFinished() {
    size_t numWriteOps = _clientRequest->sizeWriteOps();
    bool orderedOps = _clientRequest->getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        if (writeOp.getWriteState() < WriteOpState_Completed)
            return false;
        else if (orderedOps && writeOp.getWriteState() == WriteOpState_Error)
            return true;
    }

    return true;
}

//
// Aggregation functions for building the final response errors
//

void BatchWriteOp::buildClientResponse(BatchedCommandResponse* batchResp) {
    dassert(isFinished());

    // Result is OK
    batchResp->setOk(true);

    // For non-verbose, it's all we need.
    if (!_clientRequest->isVerboseWC()) {
        dassert(batchResp->isValid(NULL));
        return;
    }

    //
    // Find all the errors in the batch
    //

    vector<WriteOp*> errOps;

    size_t numWriteOps = _clientRequest->sizeWriteOps();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];

        if (writeOp.getWriteState() == WriteOpState_Error) {
            errOps.push_back(&writeOp);
        }
    }

    //
    // Build the per-item errors.
    //

    if (!errOps.empty()) {
        for (vector<WriteOp*>::iterator it = errOps.begin(); it != errOps.end(); ++it) {
            WriteOp& writeOp = **it;
            WriteErrorDetail* error = new WriteErrorDetail();
            writeOp.getOpError().cloneTo(error);
            batchResp->addToErrDetails(error);
        }
    }

    // Only return a write concern error if everything succeeded (unordered or ordered)
    // OR if something succeeded and we're unordered
    bool reportWCError = errOps.empty() ||
        (!_clientRequest->getOrdered() && errOps.size() < _clientRequest->sizeWriteOps());
    if (!_wcErrors.empty() && reportWCError) {
        WriteConcernErrorDetail* error = new WriteConcernErrorDetail;

        // Generate the multi-error message below
        stringstream msg;
        if (_wcErrors.size() > 1) {
            msg << "multiple errors reported : ";
            error->setErrCode(ErrorCodes::WriteConcernFailed);
        } else {
            error->setErrCode((*_wcErrors.begin())->error.getErrCode());
        }

        for (vector<ShardWCError*>::const_iterator it = _wcErrors.begin(); it != _wcErrors.end();
             ++it) {
            const ShardWCError* wcError = *it;
            if (it != _wcErrors.begin())
                msg << " :: and :: ";
            msg << wcError->error.getErrMessage() << " at " << wcError->endpoint.shardName;
        }

        error->setErrMessage(msg.str());
        batchResp->setWriteConcernError(error);
    }

    //
    // Append the upserted ids, if required
    //

    if (_upsertedIds.size() != 0) {
        batchResp->setUpsertDetails(_upsertedIds.vector());
    }

    // Stats
    int nValue =
        _stats->numInserted + _stats->numUpserted + _stats->numMatched + _stats->numDeleted;
    batchResp->setN(nValue);
    if (_clientRequest->getBatchType() == BatchedCommandRequest::BatchType_Update &&
        _stats->numModified >= 0) {
        batchResp->setNModified(_stats->numModified);
    }

    dassert(batchResp->isValid(NULL));
}

BatchWriteOp::~BatchWriteOp() {
    // Caller's responsibility to dispose of TargetedBatches
    dassert(_targeted.empty());

    if (NULL != _writeOps) {
        size_t numWriteOps = _clientRequest->sizeWriteOps();
        for (size_t i = 0; i < numWriteOps; ++i) {
            // Placement new so manual destruct
            _writeOps[i].~WriteOp();
        }

        ::operator delete[](_writeOps);
        _writeOps = NULL;
    }
}

int BatchWriteOp::numWriteOps() const {
    return static_cast<int>(_clientRequest->sizeWriteOps());
}

int BatchWriteOp::numWriteOpsIn(WriteOpState opState) const {
    // TODO: This could be faster, if we tracked this info explicitly
    size_t numWriteOps = _clientRequest->sizeWriteOps();
    int count = 0;
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        if (writeOp.getWriteState() == opState)
            ++count;
    }

    return count;
}

void TrackedErrors::startTracking(int errCode) {
    dassert(!isTracking(errCode));
    _errorMap.insert(make_pair(errCode, vector<ShardError*>()));
}

bool TrackedErrors::isTracking(int errCode) const {
    return _errorMap.find(errCode) != _errorMap.end();
}

void TrackedErrors::addError(ShardError* error) {
    TrackedErrorMap::iterator seenIt = _errorMap.find(error->error.getErrCode());
    if (seenIt == _errorMap.end())
        return;
    seenIt->second.push_back(error);
}

const vector<ShardError*>& TrackedErrors::getErrors(int errCode) const {
    dassert(isTracking(errCode));
    return _errorMap.find(errCode)->second;
}

void TrackedErrors::clear() {
    for (TrackedErrorMap::iterator it = _errorMap.begin(); it != _errorMap.end(); ++it) {
        vector<ShardError*>& errors = it->second;

        for (vector<ShardError*>::iterator errIt = errors.begin(); errIt != errors.end(); ++errIt) {
            delete *errIt;
        }
        errors.clear();
    }
}

TrackedErrors::~TrackedErrors() {
    clear();
}
}
