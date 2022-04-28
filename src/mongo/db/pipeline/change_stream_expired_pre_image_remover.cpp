/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "change_stream_expired_pre_image_remover.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"

namespace mongo {
// Fail point to set current time for time-based expiration of pre-images.
MONGO_FAIL_POINT_DEFINE(changeStreamPreImageRemoverCurrentTime);

namespace preImageRemoverInternal {

// Get the 'expireAfterSeconds' from the 'ChangeStreamOptions' if not 'off', boost::none otherwise.
boost::optional<std::int64_t> getExpireAfterSecondsFromChangeStreamOptions(
    ChangeStreamOptions& changeStreamOptions) {
    const stdx::variant<std::string, std::int64_t>& expireAfterSeconds =
        changeStreamOptions.getPreAndPostImages().getExpireAfterSeconds();

    if (!stdx::holds_alternative<std::string>(expireAfterSeconds)) {
        return stdx::get<std::int64_t>(expireAfterSeconds);
    }

    return boost::none;
}

// Returns pre-images expiry time in milliseconds since the epoch time if configured, boost::none
// otherwise.
boost::optional<Date_t> getPreImageExpirationTime(OperationContext* opCtx, Date_t currentTime) {
    boost::optional<std::int64_t> expireAfterSeconds = boost::none;

    // Get the expiration time directly from the change stream manager.
    auto changeStreamOptions = ChangeStreamOptionsManager::get(opCtx).getOptions(opCtx);
    expireAfterSeconds = getExpireAfterSecondsFromChangeStreamOptions(changeStreamOptions);

    // A pre-image is eligible for deletion if:
    //   pre-image's op-time + expireAfterSeconds  < currentTime.
    return expireAfterSeconds ? boost::optional<Date_t>(currentTime - Seconds(*expireAfterSeconds))
                              : boost::none;
}

}  // namespace preImageRemoverInternal

namespace {

RecordId toRecordId(ChangeStreamPreImageId id) {
    return record_id_helpers::keyForElem(
        BSON(ChangeStreamPreImage::kIdFieldName << id.toBSON()).firstElement());
}

/**
 * Finds the next collection UUID in the change stream pre-images collection 'preImagesCollPtr' for
 * which collection UUID is greater than 'collectionUUID'. Returns boost::none if the next
 * collection is not found.
 */
boost::optional<UUID> findNextCollectionUUID(OperationContext* opCtx,
                                             const CollectionPtr* preImagesCollPtr,
                                             boost::optional<UUID> collectionUUID

) {
    BSONObj preImageObj;
    auto minRecordId = collectionUUID
        ? boost::make_optional(RecordIdBound(toRecordId(ChangeStreamPreImageId(
              *collectionUUID, Timestamp::max(), std::numeric_limits<int64_t>::max()))))
        : boost::none;
    auto planExecutor =
        InternalPlanner::collectionScan(opCtx,
                                        preImagesCollPtr,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        InternalPlanner::Direction::FORWARD,
                                        boost::none /* resumeAfterRecordId */,
                                        std::move(minRecordId));
    if (planExecutor->getNext(&preImageObj, nullptr) == PlanExecutor::IS_EOF) {
        return boost::none;
    }
    auto parsedUUID = UUID::parse(preImageObj["_id"].Obj()["nsUUID"]);
    tassert(7027400, "Pre-image collection UUID must be of UUID type", parsedUUID.isOK());
    return {std::move(parsedUUID.getValue())};
}

/**
 * Scans the 'config.system.preimages' collection and deletes the expired pre-images from it.
 *
 * Pre-images are ordered by collection UUID, ie. if UUID of collection A is ordered before UUID of
 * collection B, then pre-images of collection A will be stored before pre-images of collection B.
 *
 * Pre-images are considered expired based on expiration parameter. In case when expiration
 * parameter is not set a pre-image is considered expired if its timestamp is smaller than the
 * timestamp of the earliest oplog entry. In case when expiration parameter is specified, aside from
 * timestamp check a check on the wall clock time of the pre-image recording ('operationTime') is
 * performed. If the difference between 'currentTimeForTimeBasedExpiration' and 'operationTime' is
 * larger than expiration parameter, the pre-image is considered expired. One of those two
 * conditions must be true for a pre-image to be eligible for deletion.
 *
 *                               +-------------------------+
 *                               | config.system.preimages |
 *                               +------------+------------+
 *                                            |
 *             +--------------------+---------+---------+-----------------------+
 *             |                    |                   |                       |
 * +-----------+-------+ +----------+--------+ +--------+----------+ +----------+--------+
 * |  collA.preImageA  | |  collA.preImageB  | |  collB.preImageC  | |  collB.preImageD  |
 * +-----------+-------+ +----------+--------+ +---------+---------+ +----------+--------+
 * |   timestamp: 1    | |   timestamp: 10   | |   timestamp: 5    | |   timestamp: 9    |
 * |   applyIndex: 0   | |   applyIndex: 0   | |   applyIndex: 0   | |   applyIndex: 1   |
 * +-------------------+ +-------------------+ +-------------------+ +-------------------+
 */
size_t deleteExpiredChangeStreamPreImages(OperationContext* opCtx,
                                          Date_t currentTimeForTimeBasedExpiration) {
    // Acquire intent-exclusive lock on the pre-images collection. Early exit if the collection
    // doesn't exist.
    AutoGetCollection autoColl(opCtx, NamespaceString::kChangeStreamPreImagesNamespace, MODE_IX);
    const auto& preImagesColl = autoColl.getCollection();
    if (!preImagesColl) {
        return 0;
    }

    // Do not run the job on secondaries.
    if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
            opCtx, NamespaceString::kAdminDb)) {
        return 0;
    }

    // Get the timestamp of the earliest oplog entry.
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    const bool isBatchedRemoval = gBatchedExpiredChangeStreamPreImageRemoval.load();
    size_t numberOfRemovals = 0;
    const auto preImageExpirationTime = ::mongo::preImageRemoverInternal::getPreImageExpirationTime(
        opCtx, currentTimeForTimeBasedExpiration);

    // Configure the filter for the case when expiration parameter is set.
    OrMatchExpression filter;
    const MatchExpression* filterPtr = nullptr;
    if (preImageExpirationTime) {
        filter.add(
            std::make_unique<LTMatchExpression>("_id.ts"_sd, Value(currentEarliestOplogEntryTs)));
        filter.add(std::make_unique<LTEMatchExpression>("operationTime"_sd,
                                                        Value(*preImageExpirationTime)));
        filterPtr = &filter;
    }
    const bool shouldReturnEofOnFilterMismatch = preImageExpirationTime.has_value();

    boost::optional<UUID> currentCollectionUUID = boost::none;
    while ((currentCollectionUUID =
                findNextCollectionUUID(opCtx, &preImagesColl, currentCollectionUUID))) {
        writeConflictRetry(
            opCtx,
            "ChangeStreamExpiredPreImagesRemover",
            NamespaceString::kChangeStreamPreImagesNamespace.ns(),
            [&] {
                auto params = std::make_unique<DeleteStageParams>();
                params->isMulti = true;

                std::unique_ptr<BatchedDeleteStageBatchParams> batchedDeleteParams;
                if (isBatchedRemoval) {
                    batchedDeleteParams = std::make_unique<BatchedDeleteStageBatchParams>();
                }
                RecordIdBound minRecordId(
                    toRecordId(ChangeStreamPreImageId(*currentCollectionUUID, Timestamp(), 0)));

                // If the expiration parameter is set, the 'maxRecord' is set to the maximum
                // RecordId for this collection. Whether the pre-image has to be deleted will be
                // determined by the filtering MatchExpression.
                //
                // If the expiration parameter is not set, then the last expired pre-image timestamp
                // equals to one increment before the 'currentEarliestOplogEntryTs'.
                RecordIdBound maxRecordId = RecordIdBound(toRecordId(ChangeStreamPreImageId(
                    *currentCollectionUUID,
                    preImageExpirationTime ? Timestamp::max()
                                           : Timestamp(currentEarliestOplogEntryTs.asULL() - 1),
                    std::numeric_limits<int64_t>::max())));

                auto exec = InternalPlanner::deleteWithCollectionScan(
                    opCtx,
                    &preImagesColl,
                    std::move(params),
                    PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                    InternalPlanner::Direction::FORWARD,
                    std::move(minRecordId),
                    std::move(maxRecordId),
                    CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords,
                    std::move(batchedDeleteParams),
                    filterPtr,
                    shouldReturnEofOnFilterMismatch);
                numberOfRemovals += exec->executeDelete();
            });
    }
    return numberOfRemovals;
}

void performExpiredChangeStreamPreImagesRemovalPass(Client* client) {
    ServiceContext::UniqueOperationContext opCtx;
    try {
        Date_t currentTimeForTimeBasedExpiration = Date_t::now();
        opCtx = client->makeOperationContext();

        changeStreamPreImageRemoverCurrentTime.execute([&](const BSONObj& data) {
            // Populate the current time for time based expiration of pre-images.
            if (auto currentTimeElem = data["currentTimeForTimeBasedExpiration"]) {
                const BSONType bsonType = currentTimeElem.type();
                tassert(5869300,
                        str::stream() << "Expected type for 'currentTimeForTimeBasedExpiration' is "
                                         "'date', but found: "
                                      << bsonType,
                        bsonType == BSONType::Date);

                currentTimeForTimeBasedExpiration = currentTimeElem.Date();
            }
        });
        deleteExpiredChangeStreamPreImages(opCtx.get(), currentTimeForTimeBasedExpiration);
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        LOGV2_WARNING(5869105, "Periodic expired pre-images removal job was interrupted");
    } catch (const DBException& exception) {
        LOGV2_ERROR(5869106,
                    "Periodic expired pre-images removal job failed",
                    "reason"_attr = exception.reason());
    }
}
}  // namespace

class ChangeStreamExpiredPreImagesRemover;

namespace {
const auto getChangeStreamExpiredPreImagesRemover =
    ServiceContext::declareDecoration<std::unique_ptr<ChangeStreamExpiredPreImagesRemover>>();
}  // namespace

/**
 * A periodic background job that removes expired change stream pre-image documents from the
 * 'system.preimages' collection. The period of the job is controlled by the server parameter
 * "expiredChangeStreamPreImageRemovalJobSleepSecs".
 */
class ChangeStreamExpiredPreImagesRemover : public BackgroundJob {
public:
    explicit ChangeStreamExpiredPreImagesRemover() : BackgroundJob(false /* selfDelete */) {}

    /**
     * Retrieves ChangeStreamExpiredPreImagesRemover from the service context 'serviceCtx'.
     */
    static ChangeStreamExpiredPreImagesRemover* get(ServiceContext* serviceCtx) {
        return getChangeStreamExpiredPreImagesRemover(serviceCtx).get();
    }

    /**
     * Sets ChangeStreamExpiredPreImagesRemover 'preImagesRemover' to the service context
     * 'serviceCtx'.
     */
    static void set(ServiceContext* serviceCtx,
                    std::unique_ptr<ChangeStreamExpiredPreImagesRemover> preImagesRemover) {
        auto& changeStreamExpiredPreImagesRemover =
            getChangeStreamExpiredPreImagesRemover(serviceCtx);
        if (changeStreamExpiredPreImagesRemover) {
            invariant(!changeStreamExpiredPreImagesRemover->running(),
                      "Tried to reset the ChangeStreamExpiredPreImagesRemover without shutting "
                      "down the original instance.");
        }

        invariant(preImagesRemover);
        changeStreamExpiredPreImagesRemover = std::move(preImagesRemover);
    }

    std::string name() const {
        return "ChangeStreamExpiredPreImagesRemover";
    }

    void run() {
        ThreadClient tc(name(), getGlobalServiceContext());
        AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());

        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationKillableByStepdown(lk);
        }

        while (true) {
            LOGV2_DEBUG(6278517, 3, "Thread awake");
            auto iterationStartTime = Date_t::now();
            performExpiredChangeStreamPreImagesRemovalPass(tc.get());
            {
                // Wait until either gExpiredChangeStreamPreImageRemovalJobSleepSecs passes or a
                // shutdown is requested.
                auto deadline = iterationStartTime +
                    Seconds(gExpiredChangeStreamPreImageRemovalJobSleepSecs.load());
                stdx::unique_lock<Latch> lk(_stateMutex);

                MONGO_IDLE_THREAD_BLOCK;
                _shuttingDownCV.wait_until(
                    lk, deadline.toSystemTimePoint(), [&] { return _shuttingDown; });

                if (_shuttingDown) {
                    return;
                }
            }
        }
    }

    /**
     * Signals the thread to quit and then waits until it does.
     */
    void shutdown() {
        LOGV2(6278515, "Shutting down Change Stream Expired Pre-images Remover thread");
        {
            stdx::lock_guard<Latch> lk(_stateMutex);
            _shuttingDown = true;
        }
        _shuttingDownCV.notify_one();
        wait();
        LOGV2(6278516, "Finished shutting down Change Stream Expired Pre-images Remover thread");
    }

private:
    // Protects the state below.
    mutable Mutex _stateMutex = MONGO_MAKE_LATCH("ChangeStreamExpiredPreImagesRemoverStateMutex");

    // Signaled to wake up the thread, if the thread is waiting. The thread will check whether
    // _shuttingDown is set and stop accordingly.
    mutable stdx::condition_variable _shuttingDownCV;

    bool _shuttingDown = false;
};

void startChangeStreamExpiredPreImagesRemover(ServiceContext* serviceContext) {
    std::unique_ptr<ChangeStreamExpiredPreImagesRemover> changeStreamExpiredPreImagesRemover =
        std::make_unique<ChangeStreamExpiredPreImagesRemover>();
    changeStreamExpiredPreImagesRemover->go();
    ChangeStreamExpiredPreImagesRemover::set(serviceContext,
                                             std::move(changeStreamExpiredPreImagesRemover));
}

void shutdownChangeStreamExpiredPreImagesRemover(ServiceContext* serviceContext) {
    ChangeStreamExpiredPreImagesRemover* changeStreamExpiredPreImagesRemover =
        ChangeStreamExpiredPreImagesRemover::get(serviceContext);
    // We allow the ChangeStreamExpiredPreImagesRemover not to be set in case shutdown occurs before
    // the thread has been initialized.
    if (changeStreamExpiredPreImagesRemover) {
        changeStreamExpiredPreImagesRemover->shutdown();
    }
}

}  // namespace mongo
