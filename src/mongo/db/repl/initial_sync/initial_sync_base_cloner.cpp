// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/initial_sync/initial_sync_base_cloner.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/initial_sync/repl_sync_shared_data.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync


namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeCheckingRollBackIdClonerStage);
}  // namespace
namespace repl {

// These failpoints are shared with initial_syncer and so must not be in the unnamed namespace.
MONGO_FAIL_POINT_DEFINE(initialSyncFuzzerSynchronizationPoint1);
MONGO_FAIL_POINT_DEFINE(initialSyncFuzzerSynchronizationPoint2);

InitialSyncBaseCloner::InitialSyncBaseCloner(std::string_view clonerName,
                                             InitialSyncSharedData* sharedData,
                                             const HostAndPort& source,
                                             DBClientConnection* client,
                                             StorageInterface* storageInterface,
                                             ThreadPool* dbPool)
    : BaseCloner(clonerName, sharedData, source, client, storageInterface, dbPool) {}

void InitialSyncBaseCloner::clearRetryingState() {
    _retryableOp = boost::none;
}

bool InitialSyncBaseCloner::shouldUseRawDataOperations() {
    // We can use this FCV-gated feature flag to check if raw data operations are supported, since:
    // - The in-memory FCV is always initialized, since the cloner runs after _fcvFetcherCallback.
    // - If the FCV changes during initial sync, it will fail (SERVER-31019).
    //
    // Also note that, since the choice of whether to use raw data operations depends on the FCV
    // (not the sync source's binary), we may pessimistically return `false` here.
    // This is fine, because raw data operations are only required to clone viewless timeseries
    // collections, which can only be created on an FCV that supports raw data operations.
    return gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
        kNoVersionContext, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

int InitialSyncBaseCloner::getRetryableOperationCount_forTest() {
    if (!_retryableOp) {
        return 0;
    }

    std::lock_guard<InitialSyncSharedData> lk(*_retryableOp->getSharedData());
    return _retryableOp->getSharedData()->getRetryingOperationsCount(lk);
}

void InitialSyncBaseCloner::handleStageAttemptFailed(BaseClonerStage* stage, Status lastError) {
    auto isThisStageFailPoint = [this, stage](const BSONObj& data) {
        return data["stage"].str() == stage->getName() && isMyFailPoint(data);
    };

    bool shouldRetry = [&] {
        std::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        return getSharedData()->shouldRetryOperation(lk, &_retryableOp);
    }();
    if (!shouldRetry) {
        auto status = lastError.withContext(
            str::stream() << ": Exceeded initialSyncTransientErrorRetryPeriodSeconds "
                          << getSharedData()->getAllowedOutageDuration(
                                 std::lock_guard<InitialSyncSharedData>(*getSharedData())));
        setSyncFailedStatus(status);
        uassertStatusOK(status);
    }
    hangBeforeCheckingRollBackIdClonerStage.executeIf(
        [&](const BSONObj& data) {
            LOGV2(21076,
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
        initialSyncId =
            getClient()->findOne(NamespaceString::kDefaultInitialSyncIdNamespace, BSONObj{});
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
        InitialSyncIdDocument::parse(initialSyncId, IDLParserContext("initialSyncId"));

    std::lock_guard<ReplSyncSharedData> lk(*getSharedData());
    uassert(ErrorCodes::InitialSyncFailure,
            "Sync source has been resynced since we started syncing from it",
            getSharedData()->getInitialSyncSourceId(lk) == initialSyncIdDoc.get_id());
    return Status::OK();
}

Status InitialSyncBaseCloner::checkRollBackIdIsUnchanged() {
    BSONObj info;
    try {
        getClient()->runCommand(DatabaseName::kAdmin, BSON("replSetGetRBID" << 1), info);
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
