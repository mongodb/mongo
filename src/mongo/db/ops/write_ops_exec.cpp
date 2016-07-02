/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/checked_cast.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/introspect.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/command_reply.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/command_request_builder.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

// Convention in this file: generic helpers go in the anonymous namespace. Helpers that are for a
// single type of operation are static functions defined above their caller.
namespace {

void finishCurOp(OperationContext* txn, CurOp* curOp) {
    try {
        curOp->done();
        int executionTimeMs = curOp->totalTimeMillis();
        curOp->debug().executionTime = executionTimeMs;

        recordCurOpMetrics(txn);
        Top::get(txn->getServiceContext())
            .record(txn,
                    curOp->getNS(),
                    curOp->getLogicalOp(),
                    1,  // "write locked"
                    curOp->totalTimeMicros(),
                    curOp->isCommand(),
                    curOp->getReadWriteType());

        if (!curOp->debug().exceptionInfo.empty()) {
            LOG(3) << "Caught Assertion in " << logicalOpToString(curOp->getLogicalOp()) << ": "
                   << curOp->debug().exceptionInfo.toString();
        }

        const bool logAll = logger::globalLogDomain()->shouldLog(logger::LogComponent::kCommand,
                                                                 logger::LogSeverity::Debug(1));
        const bool logSlow =
            executionTimeMs > (serverGlobalParams.slowMS + curOp->getExpectedLatencyMs());

        if (logAll || logSlow) {
            Locker::LockerInfo lockerInfo;
            txn->lockState()->getLockerInfo(&lockerInfo);
            log() << curOp->debug().report(*curOp, lockerInfo.stats);
        }

        if (curOp->shouldDBProfile(executionTimeMs)) {
            profile(txn, CurOp::get(txn)->getNetworkOp());
        }
    } catch (const DBException& ex) {
        // We need to ignore all errors here. We don't want a successful op to fail because of a
        // failure to record stats. We also don't want to replace the error reported for an op that
        // is failing.
        log() << "Ignoring error from finishCurOp: " << ex.toString();
    }
}

/**
 * Sets the Client's LastOp to the system OpTime if needed.
 */
class LastOpFixer {
public:
    LastOpFixer(OperationContext* txn, const NamespaceString& ns)
        : _txn(txn), _isOnLocalDb(ns.isLocal()) {}

    ~LastOpFixer() {
        if (_needToFixLastOp && !_isOnLocalDb) {
            // If this operation has already generated a new lastOp, don't bother setting it
            // here. No-op updates will not generate a new lastOp, so we still need the
            // guard to fire in that case. Operations on the local DB aren't replicated, so they
            // don't need to bump the lastOp.
            replClientInfo().setLastOpToSystemLastOpTime(_txn);
        }
    }

    void startingOp() {
        _needToFixLastOp = true;
        _opTimeAtLastOpStart = replClientInfo().getLastOp();
    }

    void finishedOpSuccessfully() {
        // If the op was succesful and bumped LastOp, we don't need to do it again. However, we
        // still need to for no-ops and all failing ops.
        _needToFixLastOp = (replClientInfo().getLastOp() == _opTimeAtLastOpStart);
    }

private:
    repl::ReplClientInfo& replClientInfo() {
        return repl::ReplClientInfo::forClient(_txn->getClient());
    }

    OperationContext* const _txn;
    bool _needToFixLastOp = true;
    const bool _isOnLocalDb;
    repl::OpTime _opTimeAtLastOpStart;
};

void assertCanWrite_inlock(OperationContext* txn, const NamespaceString& ns) {
    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while writing to " << ns.ns(),
            repl::ReplicationCoordinator::get(txn->getServiceContext())->canAcceptWritesFor(ns));
    CollectionShardingState::get(txn, ns)->checkShardVersionOrThrow(txn);
}

void makeCollection(OperationContext* txn, const NamespaceString& ns) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetOrCreateDb db(txn, ns.db(), MODE_X);
        assertCanWrite_inlock(txn, ns);
        if (!db.getDb()->getCollection(ns.ns())) {  // someone else may have beat us to it.
            WriteUnitOfWork wuow(txn);
            uassertStatusOK(userCreateNS(txn, db.getDb(), ns.ns(), BSONObj()));
            wuow.commit();
        }
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "implicit collection creation", ns.ns());
}

/**
 * Returns true if the operation can continue.
 */
bool handleError(OperationContext* txn,
                 const DBException& ex,
                 const ParsedWriteOp& wholeOp,
                 WriteResult* out) {
    LastError::get(txn->getClient()).setLastError(ex.getCode(), ex.getInfo().msg);
    auto& curOp = *CurOp::get(txn);
    curOp.debug().exceptionInfo = ex.getInfo();

    if (ErrorCodes::isInterruption(ErrorCodes::Error(ex.getCode()))) {
        throw;  // These have always failed the whole batch.
    }

    if (ErrorCodes::isStaleShardingError(ErrorCodes::Error(ex.getCode()))) {
        auto staleConfigException = dynamic_cast<const SendStaleConfigException*>(&ex);
        if (!staleConfigException) {
            // We need to get extra info off of the SCE, but some common patterns can result in the
            // exception being converted to a Status then rethrown as a UserException, losing the
            // info we need. It would be a bug if this happens so we want to detect it in testing,
            // but it isn't severe enough that we should bring down the server if it happens in
            // production.
            dassert(staleConfigException);
            msgassertedNoTrace(35475,
                               str::stream()
                                   << "Got a StaleConfig error but exception was the wrong type: "
                                   << demangleName(typeid(ex)));
        }

        ShardingState::get(txn)->onStaleShardVersion(
            txn, wholeOp.ns, staleConfigException->getVersionReceived());
        out->staleConfigException =
            stdx::make_unique<SendStaleConfigException>(*staleConfigException);
        return false;
    }

    out->results.emplace_back(ex.toStatus());
    return wholeOp.continueOnError;
}

}  // namespace

static WriteResult::SingleResult createIndex(OperationContext* txn,
                                             const NamespaceString& systemIndexes,
                                             const BSONObj& spec) {
    BSONElement nsElement = spec["ns"];
    uassert(ErrorCodes::NoSuchKey, "Missing \"ns\" field in index description", !nsElement.eoo());
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Expected \"ns\" field of index description to be a "
                             "string, "
                             "but found a "
                          << typeName(nsElement.type()),
            nsElement.type() == String);
    const NamespaceString ns(nsElement.valueStringData());
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Cannot create an index on " << ns.ns() << " with an insert to "
                          << systemIndexes.ns(),
            ns.db() == systemIndexes.db());

    BSONObjBuilder cmdBuilder;
    cmdBuilder << "createIndexes" << ns.coll();
    cmdBuilder << "indexes" << BSON_ARRAY(spec);
    BSONObj cmd = cmdBuilder.done();

    rpc::CommandRequestBuilder requestBuilder;
    auto cmdRequestMsg = requestBuilder.setDatabase(ns.db())
                             .setCommandName("createIndexes")
                             .setCommandArgs(cmd)
                             .setMetadata(rpc::makeEmptyMetadata())
                             .done();
    rpc::CommandRequest cmdRequest(&cmdRequestMsg);
    rpc::CommandReplyBuilder cmdReplyBuilder;
    Command::findCommand("createIndexes")->run(txn, cmdRequest, &cmdReplyBuilder);
    auto cmdReplyMsg = cmdReplyBuilder.done();
    rpc::CommandReply cmdReply(&cmdReplyMsg);
    auto cmdResult = cmdReply.getCommandReply();
    uassertStatusOK(getStatusFromCommandResult(cmdResult));

    // Unlike normal inserts, it is not an error to "insert" a duplicate index.
    long long n =
        cmdResult["numIndexesAfter"].numberInt() - cmdResult["numIndexesBefore"].numberInt();
    CurOp::get(txn)->debug().ninserted += n;

    return {n};
}

static WriteResult performCreateIndexes(OperationContext* txn, const InsertOp& wholeOp) {
    // Currently this creates each index independently. We could pass multiple indexes to
    // createIndexes, but there is a lot of complexity involved in doing it correctly. For one
    // thing, createIndexes only takes indexes to a single collection, but this batch could include
    // different collections. Additionally, the error handling is different: createIndexes is
    // all-or-nothing while inserts are supposed to behave like a sequence that either skips over
    // errors or stops at the first one. These could theoretically be worked around, but it doesn't
    // seem worth it since users that want faster index builds should just use the createIndexes
    // command rather than a legacy emulation.
    LastOpFixer lastOpFixer(txn, wholeOp.ns);
    WriteResult out;
    for (auto&& spec : wholeOp.documents) {
        try {
            lastOpFixer.startingOp();
            out.results.emplace_back(createIndex(txn, wholeOp.ns, spec));
            lastOpFixer.finishedOpSuccessfully();
        } catch (const DBException& ex) {
            const bool canContinue = handleError(txn, ex, wholeOp, &out);
            if (!canContinue)
                break;
        }
    }
    return out;
}

static void insertDocuments(OperationContext* txn,
                            Collection* collection,
                            std::vector<BSONObj>::const_iterator begin,
                            std::vector<BSONObj>::const_iterator end) {
    // Intentionally not using a WRITE_CONFLICT_RETRY_LOOP. That is handled by the caller so it can
    // react to oversized batches.
    WriteUnitOfWork wuow(txn);
    uassertStatusOK(collection->insertDocuments(
        txn, begin, end, &CurOp::get(txn)->debug(), /*enforceQuota*/ true));
    wuow.commit();
}

/**
 * Returns true if caller should try to insert more documents. Does nothing else if batch is empty.
 */
static bool insertBatchAndHandleErrors(OperationContext* txn,
                                       const InsertOp& wholeOp,
                                       const std::vector<BSONObj>& batch,
                                       LastOpFixer* lastOpFixer,
                                       WriteResult* out) {
    if (batch.empty())
        return true;

    auto& curOp = *CurOp::get(txn);

    boost::optional<AutoGetCollection> collection;
    auto acquireCollection = [&] {
        while (true) {
            txn->checkForInterrupt();
            collection.emplace(txn, wholeOp.ns, MODE_IX);
            if (collection->getCollection())
                break;

            collection.reset();  // unlock.
            makeCollection(txn, wholeOp.ns);
        }

        curOp.raiseDbProfileLevel(collection->getDb()->getProfilingLevel());
        assertCanWrite_inlock(txn, wholeOp.ns);
    };

    try {
        acquireCollection();
        if (!collection->getCollection()->isCapped() && batch.size() > 1) {
            // First try doing it all together. If all goes well, this is all we need to do.
            // See Collection::_insertDocuments for why we do all capped inserts one-at-a-time.
            lastOpFixer->startingOp();
            insertDocuments(txn, collection->getCollection(), batch.begin(), batch.end());
            lastOpFixer->finishedOpSuccessfully();
            globalOpCounters.gotInserts(batch.size());
            std::fill_n(
                std::back_inserter(out->results), batch.size(), WriteResult::SingleResult{1});
            curOp.debug().ninserted += batch.size();
            return true;
        }
    } catch (const DBException& ex) {
        collection.reset();
        // Ignore this failure and behave as-if we never tried to do the combined batch insert.
        // The loop below will handle reporting any non-transient errors.
    }

    // Try to insert the batch one-at-a-time. This path is executed both for singular batches, and
    // for batches that failed all-at-once inserting.
    for (auto it = batch.begin(); it != batch.end(); ++it) {
        globalOpCounters.gotInsert();
        try {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                try {
                    if (!collection)
                        acquireCollection();
                    lastOpFixer->startingOp();
                    insertDocuments(txn, collection->getCollection(), it, it + 1);
                    lastOpFixer->finishedOpSuccessfully();
                    out->results.emplace_back(WriteResult::SingleResult{1});
                    curOp.debug().ninserted++;
                } catch (...) {
                    // Release the lock following any error. Among other things, this ensures that
                    // we don't sleep in the WCE retry loop with the lock held.
                    collection.reset();
                    throw;
                }
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "insert", wholeOp.ns.ns());
        } catch (const DBException& ex) {
            bool canContinue = handleError(txn, ex, wholeOp, out);
            if (!canContinue)
                return false;
        }
    }

    return true;
}

WriteResult performInserts(OperationContext* txn, const InsertOp& wholeOp) {
    invariant(!txn->lockState()->inAWriteUnitOfWork());  // Does own retries.
    auto& curOp = *CurOp::get(txn);
    ON_BLOCK_EXIT([&] {
        // This is the only part of finishCurOp we need to do for inserts because they reuse the
        // top-level curOp. The rest is handled by the top-level entrypoint.
        curOp.done();
        Top::get(txn->getServiceContext())
            .record(txn,
                    wholeOp.ns.ns(),
                    LogicalOp::opInsert,
                    1 /* write locked*/,
                    curOp.totalTimeMicros(),
                    curOp.isCommand(),
                    curOp.getReadWriteType());

    });

    {
        stdx::lock_guard<Client>(*txn->getClient());
        curOp.setNS_inlock(wholeOp.ns.ns());
        curOp.setLogicalOp_inlock(LogicalOp::opInsert);
        curOp.ensureStarted();
        curOp.debug().ninserted = 0;
    }

    uassertStatusOK(userAllowedWriteNS(wholeOp.ns));

    if (wholeOp.ns.isSystemDotIndexes()) {
        return performCreateIndexes(txn, wholeOp);
    }

    DisableDocumentValidationIfTrue docValidationDisabler(txn, wholeOp.bypassDocumentValidation);
    LastOpFixer lastOpFixer(txn, wholeOp.ns);

    WriteResult out;
    out.results.reserve(wholeOp.documents.size());

    size_t bytesInBatch = 0;
    std::vector<BSONObj> batch;
    const size_t maxBatchSize = internalQueryExecYieldIterations / 2;
    batch.reserve(std::min(wholeOp.documents.size(), maxBatchSize));

    for (auto&& doc : wholeOp.documents) {
        const bool isLastDoc = (&doc == &wholeOp.documents.back());
        auto fixedDoc = fixDocumentForInsert(doc);
        if (!fixedDoc.isOK()) {
            // Handled after we insert anything in the batch to be sure we report errors in the
            // correct order. In an ordered insert, if one of the docs ahead of us fails, we should
            // behave as-if we never got to this document.
        } else {
            batch.push_back(fixedDoc.getValue().isEmpty() ? doc : std::move(fixedDoc.getValue()));
            bytesInBatch += batch.back().objsize();
            if (!isLastDoc && batch.size() < maxBatchSize && bytesInBatch < insertVectorMaxBytes)
                continue;  // Add more to batch before inserting.
        }

        bool canContinue = insertBatchAndHandleErrors(txn, wholeOp, batch, &lastOpFixer, &out);
        batch.clear();  // We won't need the current batch any more.
        bytesInBatch = 0;

        if (canContinue && !fixedDoc.isOK()) {
            globalOpCounters.gotInsert();
            canContinue = handleError(
                txn,
                UserException(fixedDoc.getStatus().code(), fixedDoc.getStatus().reason()),
                wholeOp,
                &out);
        }

        if (!canContinue)
            break;
    }

    return out;
}

static WriteResult::SingleResult performSingleUpdateOp(OperationContext* txn,
                                                       const NamespaceString& ns,
                                                       const UpdateOp::SingleUpdate& op) {
    globalOpCounters.gotUpdate();
    auto& curOp = *CurOp::get(txn);
    {
        stdx::lock_guard<Client> lk(*txn->getClient());
        curOp.setNS_inlock(ns.ns());
        curOp.setNetworkOp_inlock(dbUpdate);
        curOp.setLogicalOp_inlock(LogicalOp::opUpdate);
        curOp.setQuery_inlock(op.query);
        curOp.ensureStarted();
    }

    UpdateLifecycleImpl updateLifecycle(ns);
    UpdateRequest request(ns);
    request.setLifecycle(&updateLifecycle);
    request.setQuery(op.query);
    request.setCollation(op.collation);
    request.setUpdates(op.update);
    request.setMulti(op.multi);
    request.setUpsert(op.upsert);
    request.setYieldPolicy(PlanExecutor::YIELD_AUTO);  // ParsedUpdate overrides this for $isolated.

    ParsedUpdate parsedUpdate(txn, &request);
    uassertStatusOK(parsedUpdate.parseRequest());

    ScopedTransaction scopedXact(txn, MODE_IX);
    boost::optional<AutoGetCollection> collection;
    while (true) {
        txn->checkForInterrupt();
        collection.emplace(txn,
                           ns,
                           MODE_IX,  // DB is always IX, even if collection is X.
                           parsedUpdate.isIsolated() ? MODE_X : MODE_IX);
        if (collection->getCollection() || !op.upsert)
            break;

        collection.reset();  // unlock.
        makeCollection(txn, ns);
    }

    if (collection->getDb()) {
        curOp.raiseDbProfileLevel(collection->getDb()->getProfilingLevel());
    }

    assertCanWrite_inlock(txn, ns);

    auto exec = uassertStatusOK(
        getExecutorUpdate(txn, &curOp.debug(), collection->getCollection(), &parsedUpdate));

    {
        stdx::lock_guard<Client>(*txn->getClient());
        CurOp::get(txn)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
    }

    uassertStatusOK(exec->executePlan());

    PlanSummaryStats summary;
    Explain::getSummaryStats(*exec, &summary);
    if (collection->getCollection()) {
        collection->getCollection()->infoCache()->notifyOfQuery(txn, summary.indexesUsed);
    }

    if (curOp.shouldDBProfile(curOp.elapsedMillis())) {
        BSONObjBuilder execStatsBob;
        Explain::getWinningPlanStats(exec.get(), &execStatsBob);
        curOp.debug().execStats = execStatsBob.obj();
    }

    const UpdateStats* updateStats = UpdateStage::getUpdateStats(exec.get());
    UpdateStage::recordUpdateStatsInOpDebug(updateStats, &curOp.debug());
    curOp.debug().setPlanSummaryMetrics(summary);
    UpdateResult res = UpdateStage::makeUpdateResult(updateStats);

    const bool didInsert = !res.upserted.isEmpty();
    const long long nMatchedOrInserted = didInsert ? 1 : res.numMatched;
    LastError::get(txn->getClient()).recordUpdate(res.existing, nMatchedOrInserted, res.upserted);

    return {nMatchedOrInserted, res.numDocsModified, res.upserted};
}

WriteResult performUpdates(OperationContext* txn, const UpdateOp& wholeOp) {
    invariant(!txn->lockState()->inAWriteUnitOfWork());  // Does own retries.
    uassertStatusOK(userAllowedWriteNS(wholeOp.ns));

    DisableDocumentValidationIfTrue docValidationDisabler(txn, wholeOp.bypassDocumentValidation);
    LastOpFixer lastOpFixer(txn, wholeOp.ns);

    WriteResult out;
    out.results.reserve(wholeOp.updates.size());
    for (auto&& singleOp : wholeOp.updates) {
        // TODO: don't create nested CurOp for legacy writes.
        // Add Command pointer to the nested CurOp.
        auto& parentCurOp = *CurOp::get(txn);
        Command* cmd = parentCurOp.getCommand();
        CurOp curOp(txn);
        {
            stdx::lock_guard<Client>(*txn->getClient());
            curOp.setCommand_inlock(cmd);
        }
        ON_BLOCK_EXIT([&] { finishCurOp(txn, &curOp); });
        try {
            lastOpFixer.startingOp();
            out.results.emplace_back(performSingleUpdateOp(txn, wholeOp.ns, singleOp));
            lastOpFixer.finishedOpSuccessfully();
        } catch (const DBException& ex) {
            const bool canContinue = handleError(txn, ex, wholeOp, &out);
            if (!canContinue)
                break;
        }
    }

    return out;
}

static WriteResult::SingleResult performSingleDeleteOp(OperationContext* txn,
                                                       const NamespaceString& ns,
                                                       const DeleteOp::SingleDelete& op) {
    globalOpCounters.gotDelete();
    auto& curOp = *CurOp::get(txn);
    {
        stdx::lock_guard<Client> lk(*txn->getClient());
        curOp.setNS_inlock(ns.ns());
        curOp.setNetworkOp_inlock(dbDelete);
        curOp.setLogicalOp_inlock(LogicalOp::opDelete);
        curOp.setQuery_inlock(op.query);
        curOp.ensureStarted();
    }

    curOp.debug().ndeleted = 0;

    txn->checkForInterrupt();

    DeleteRequest request(ns);
    request.setQuery(op.query);
    request.setCollation(op.collation);
    request.setMulti(op.multi);
    request.setYieldPolicy(PlanExecutor::YIELD_AUTO);  // ParsedDelete overrides this for $isolated.

    ParsedDelete parsedDelete(txn, &request);
    uassertStatusOK(parsedDelete.parseRequest());

    ScopedTransaction scopedXact(txn, MODE_IX);
    AutoGetCollection collection(txn,
                                 ns,
                                 MODE_IX,  // DB is always IX, even if collection is X.
                                 parsedDelete.isIsolated() ? MODE_X : MODE_IX);
    if (collection.getDb()) {
        curOp.raiseDbProfileLevel(collection.getDb()->getProfilingLevel());
    }

    assertCanWrite_inlock(txn, ns);

    auto exec = uassertStatusOK(
        getExecutorDelete(txn, &curOp.debug(), collection.getCollection(), &parsedDelete));

    {
        stdx::lock_guard<Client>(*txn->getClient());
        CurOp::get(txn)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
    }

    uassertStatusOK(exec->executePlan());
    long long n = DeleteStage::getNumDeleted(*exec);
    curOp.debug().ndeleted = n;

    PlanSummaryStats summary;
    Explain::getSummaryStats(*exec, &summary);
    if (collection.getCollection()) {
        collection.getCollection()->infoCache()->notifyOfQuery(txn, summary.indexesUsed);
    }
    curOp.debug().setPlanSummaryMetrics(summary);

    if (curOp.shouldDBProfile(curOp.elapsedMillis())) {
        BSONObjBuilder execStatsBob;
        Explain::getWinningPlanStats(exec.get(), &execStatsBob);
        curOp.debug().execStats = execStatsBob.obj();
    }

    LastError::get(txn->getClient()).recordDelete(n);

    return {n};
}

WriteResult performDeletes(OperationContext* txn, const DeleteOp& wholeOp) {
    invariant(!txn->lockState()->inAWriteUnitOfWork());  // Does own retries.
    uassertStatusOK(userAllowedWriteNS(wholeOp.ns));

    DisableDocumentValidationIfTrue docValidationDisabler(txn, wholeOp.bypassDocumentValidation);
    LastOpFixer lastOpFixer(txn, wholeOp.ns);

    WriteResult out;
    out.results.reserve(wholeOp.deletes.size());
    for (auto&& singleOp : wholeOp.deletes) {
        // TODO: don't create nested CurOp for legacy writes.
        // Add Command pointer to the nested CurOp.
        auto& parentCurOp = *CurOp::get(txn);
        Command* cmd = parentCurOp.getCommand();
        CurOp curOp(txn);
        {
            stdx::lock_guard<Client>(*txn->getClient());
            curOp.setCommand_inlock(cmd);
        }
        ON_BLOCK_EXIT([&] { finishCurOp(txn, &curOp); });
        try {
            lastOpFixer.startingOp();
            out.results.emplace_back(performSingleDeleteOp(txn, wholeOp.ns, singleOp));
            lastOpFixer.finishedOpSuccessfully();
        } catch (const DBException& ex) {
            const bool canContinue = handleError(txn, ex, wholeOp, &out);
            if (!canContinue)
                break;
        }
    }

    return out;
}

}  // namespace mongo
