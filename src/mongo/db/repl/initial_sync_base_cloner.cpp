/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/db/repl/initial_sync_base_cloner.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync


namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeCheckingRollBackIdClonerStage);
}  // namespace
namespace repl {

// These failpoints are shared with initial_syncer and so must not be in the unnamed namespace.
MONGO_FAIL_POINT_DEFINE(initialSyncFuzzerSynchronizationPoint1);
MONGO_FAIL_POINT_DEFINE(initialSyncFuzzerSynchronizationPoint2);

InitialSyncBaseCloner::InitialSyncBaseCloner(StringData clonerName,
                                             InitialSyncSharedData* sharedData,
                                             const HostAndPort& source,
                                             DBClientConnection* client,
                                             StorageInterface* storageInterface,
                                             ThreadPool* dbPool)
    : BaseCloner(clonerName, sharedData, source, client, storageInterface, dbPool) {}

void InitialSyncBaseCloner::clearRetryingState() {
    _retryableOp = boost::none;
}

void InitialSyncBaseCloner::handleStageAttemptFailed(BaseClonerStage* stage, Status lastError) {
    auto isThisStageFailPoint = [this, stage](const BSONObj& data) {
        return data["stage"].str() == stage->getName() && isMyFailPoint(data);
    };

    bool shouldRetry = [&] {
        stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        return getSharedData()->shouldRetryOperation(lk, &_retryableOp);
    }();
    if (!shouldRetry) {
        auto status = lastError.withContext(
            str::stream() << ": Exceeded initialSyncTransientErrorRetryPeriodSeconds "
                          << getSharedData()->getAllowedOutageDuration(
                                 stdx::lock_guard<InitialSyncSharedData>(*getSharedData())));
        setSyncFailedStatus(status);
        uassertStatusOK(status);
    }
    hangBeforeCheckingRollBackIdClonerStage.executeIf(
        [&](const BSONObj& data) {
            LOGV2(21076,
                  "Initial sync cloner {cloner} hanging before checking rollBackId for stage "
                  "{stage}",
                  "Initial sync cloner hanging before checking rollBackId",
                  "cloner"_attr = getClonerName(),
                  "stage"_attr = stage->getName());
            while (!mustExit() &&
                   hangBeforeCheckingRollBackIdClonerStage.shouldFail(isThisStageFailPoint)) {
                sleepmillis(100);
            }
        },
        isThisStageFailPoint);

    // This includes checking the sync source member state, checking the rollback ID,
    // and checking the sync source initial sync ID.
    if (stage->checkSyncSourceValidityOnRetry()) {
        // If checkSyncSourceIsStillValid fails without throwing, it means a network
        // error occurred and it's safe to continue (which will cause another retry).
        if (!checkSyncSourceIsStillValid().isOK())
            return;
        // After successfully checking the sync source validity, the client should
        // always be OK.
        invariant(!getClient()->isFailed());
    }
}

Status InitialSyncBaseCloner::checkSyncSourceIsStillValid() {
    auto status = checkInitialSyncIdIsUnchanged();
    if (!status.isOK())
        return status;

    return checkRollBackIdIsUnchanged();
}

Status InitialSyncBaseCloner::checkInitialSyncIdIsUnchanged() {
    BSONObj initialSyncId;
    try {
        initialSyncId = getClient()->findOne(
            NamespaceString{ReplicationConsistencyMarkersImpl::kDefaultInitialSyncIdNamespace},
            BSONObj{});
    } catch (DBException& e) {
        if (ErrorCodes::isRetriableError(e)) {
            auto status = e.toStatus().withContext(
                ": failed while attempting to retrieve initial sync ID after re-connect");
            LOGV2_DEBUG(
                4608505, 1, "Retrieving Initial Sync ID retriable error", "error"_attr = status);
            return status;
        }
        throw;
    }
    uassert(ErrorCodes::InitialSyncFailure,
            "Cannot retrieve sync source initial sync ID",
            !initialSyncId.isEmpty());
    InitialSyncIdDocument initialSyncIdDoc =
        InitialSyncIdDocument::parse(IDLParserContext("initialSyncId"), initialSyncId);

    stdx::lock_guard<ReplSyncSharedData> lk(*getSharedData());
    uassert(ErrorCodes::InitialSyncFailure,
            "Sync source has been resynced since we started syncing from it",
            getSharedData()->getInitialSyncSourceId(lk) == initialSyncIdDoc.get_id());
    return Status::OK();
}

Status InitialSyncBaseCloner::checkRollBackIdIsUnchanged() {
    BSONObj info;
    try {
        getClient()->runCommand("admin", BSON("replSetGetRBID" << 1), info);
    } catch (DBException& e) {
        if (ErrorCodes::isRetriableError(e)) {
            static constexpr char errorMsg[] =
                "Failed while attempting to retrieve rollBackId after re-connect";
            LOGV2_DEBUG(21073, 1, errorMsg, "error"_attr = e);
            return e.toStatus().withContext(errorMsg);
        }
        throw;
    }
    uassert(
        31298, "Sync source returned invalid result from replSetGetRBID", info["rbid"].isNumber());
    auto rollBackId = info["rbid"].numberInt();
    uassert(ErrorCodes::UnrecoverableRollbackError,
            str::stream() << "Rollback occurred on our sync source " << getSource()
                          << " during initial sync",
            rollBackId == getSharedData()->getRollBackId());
    return Status::OK();
}

void InitialSyncBaseCloner::pauseForFuzzer(BaseClonerStage* stage) {
    // These are the stages that the initial sync fuzzer expects to be able to pause on using the
    // syncronization fail points.
    static const auto initialSyncPauseStages =
        std::vector<std::string>{"listCollections", "listIndexes", "listDatabases"};

    if (MONGO_unlikely(initialSyncFuzzerSynchronizationPoint1.shouldFail())) {
        if (std::find(initialSyncPauseStages.begin(),
                      initialSyncPauseStages.end(),
                      stage->getName()) != initialSyncPauseStages.end()) {
            // These failpoints are set and unset by the InitialSyncTest fixture to cause initial
            // sync to pause so that the Initial Sync Fuzzer can run commands on the sync source.
            // nb: This log message is specifically checked for in
            // initial_sync_test_fixture_test.js, so if you change it here you will need to change
            // it there.
            LOGV2(21066,
                  "Collection Cloner scheduled a remote command on the {stage}",
                  "Collection Cloner scheduled a remote command",
                  "stage"_attr = describeForFuzzer(stage));
            LOGV2(21067, "initialSyncFuzzerSynchronizationPoint1 fail point enabled");
            initialSyncFuzzerSynchronizationPoint1.pauseWhileSet();

            if (MONGO_unlikely(initialSyncFuzzerSynchronizationPoint2.shouldFail())) {
                LOGV2(21068, "initialSyncFuzzerSynchronizationPoint2 fail point enabled");
                initialSyncFuzzerSynchronizationPoint2.pauseWhileSet();
            }
        }
    }
}

logv2::LogComponent InitialSyncBaseCloner::getLogComponent() {
    return logv2::LogComponent::kReplicationInitialSync;
}

}  // namespace repl
}  // namespace mongo
