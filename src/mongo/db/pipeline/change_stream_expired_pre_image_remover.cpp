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

bool PreImageAttributes::isExpiredPreImage(const boost::optional<Date_t>& preImageExpirationTime,
                                           const Timestamp& earliestOplogEntryTimestamp) {
    // Pre-image oplog entry is no longer present in the oplog if its timestamp is smaller
    // than the 'earliestOplogEntryTimestamp'.
    const bool preImageOplogEntryIsDeleted = ts < earliestOplogEntryTimestamp;
    const auto expirationTime = preImageExpirationTime.get_value_or(Date_t::min());

    // Pre-image is expired if its corresponding oplog entry is deleted or its operation
    // time is less than or equal to the expiration time.
    return preImageOplogEntryIsDeleted || operationTime <= expirationTime;
}

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
 * Scans the 'config.system.preimages' collection and deletes the expired pre-images from it.
 *
 * Pre-images are ordered by collection UUID, ie. if UUID of collection A is ordered before UUID of
 * collection B, then pre-images of collection A will be stored before pre-images of collection B.
 *
 * While scanning the collection for expired pre-images, each pre-image timestamp is compared
 * against the 'earliestOplogEntryTimestamp' value. Any pre-image that has a timestamp greater than
 * the 'earliestOplogEntryTimestamp' value is not considered for deletion and the cursor seeks to
 * the next UUID in the collection.
 *
 * Seek to the next UUID is done by setting the values of 'Timestamp' and 'ApplyOpsIndex' fields to
 * max, ie. (currentPreImage.nsUUID, Timestamp::max(), ApplyOpsIndex::max()).
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
class ChangeStreamExpiredPreImageIterator {
public:
    // Iterator over the range of pre-image documents, where each range defines a set of expired
    // pre-image documents of one collection eligible for deletion due to expiration. Lower and
    // upper bounds of a range are inclusive.
    class Iterator {
    public:
        using RecordIdRange = std::pair<RecordId, RecordId>;

        Iterator(OperationContext* opCtx,
                 const CollectionPtr* preImagesCollPtr,
                 Timestamp earliestOplogEntryTimestamp,
                 boost::optional<Date_t> preImageExpirationTime,
                 bool isEndIterator = false)
            : _opCtx(opCtx),
              _preImagesCollPtr(preImagesCollPtr),
              _earliestOplogEntryTimestamp(earliestOplogEntryTimestamp),
              _preImageExpirationTime(preImageExpirationTime) {
            if (!isEndIterator) {
                advance();
            }
        }

        const RecordIdRange& operator*() const {
            return _currentExpiredPreImageRange;
        }

        const RecordIdRange* operator->() const {
            return &_currentExpiredPreImageRange;
        }

        Iterator& operator++() {
            advance();
            return *this;
        }

        // Both iterators are equal if they are both pointing to the same expired pre-image range.
        friend bool operator==(const Iterator& a, const Iterator& b) {
            return a._currentExpiredPreImageRange == b._currentExpiredPreImageRange;
        };

        friend bool operator!=(const Iterator& a, const Iterator& b) {
            return !(a == b);
        };

    private:
        // Scans the pre-images collection and gets the next expired pre-image range or sets
        // '_currentExpiredPreImageRange' to the range with empty record ids in case there are no
        // more expired pre-images left.
        void advance() {
            const auto getNextPreImageAttributes =
                [&](std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>& planExecutor)
                -> boost::optional<preImageRemoverInternal::PreImageAttributes> {
                BSONObj preImageObj;
                if (planExecutor->getNext(&preImageObj, nullptr) == PlanExecutor::IS_EOF) {
                    return boost::none;
                }

                auto preImage =
                    ChangeStreamPreImage::parse(IDLParserErrorContext("pre-image"), preImageObj);
                return {{std::move(preImage.getId().getNsUUID()),
                         std::move(preImage.getId().getTs()),
                         std::move(preImage.getOperationTime())}};
            };

            while (true) {
                // Fetch the first pre-image from the next collection, that has pre-images enabled.
                auto planExecutor = _previousCollectionUUID
                    ? createCollectionScan(RecordIdBound(
                          toRecordId(ChangeStreamPreImageId(*_previousCollectionUUID,
                                                            Timestamp::max(),
                                                            std::numeric_limits<int64_t>::max()))))
                    : createCollectionScan(boost::none);
                auto preImageAttributes = getNextPreImageAttributes(planExecutor);

                // If there aren't any pre-images left, set the range to the empty record ids and
                // return.
                if (!preImageAttributes) {
                    _currentExpiredPreImageRange = std::pair(RecordId(), RecordId());
                    return;
                }
                const auto currentCollectionUUID = preImageAttributes->collectionUUID;
                _previousCollectionUUID = currentCollectionUUID;

                // If the first pre-image in the current collection is not expired, fetch the first
                // pre-image from the next collection.
                if (!preImageAttributes->isExpiredPreImage(_preImageExpirationTime,
                                                           _earliestOplogEntryTimestamp)) {
                    continue;
                }

                // If an expired pre-image is found, compute the max expired pre-image RecordId for
                // this collection depending on the expiration parameter being set.
                const auto minKey =
                    toRecordId(ChangeStreamPreImageId(currentCollectionUUID, Timestamp(), 0));
                RecordId maxKey;
                if (_preImageExpirationTime) {
                    // Reset the collection scan to start one increment before the
                    // '_earliestOplogEntryTimestamp', as the pre-images with smaller or equal
                    // timestamp are guaranteed to be expired.
                    Timestamp lastExpiredPreimageTs(_earliestOplogEntryTimestamp.asULL() - 1);
                    auto planExecutor = createCollectionScan(RecordIdBound(
                        toRecordId(ChangeStreamPreImageId(currentCollectionUUID,
                                                          lastExpiredPreimageTs,
                                                          std::numeric_limits<int64_t>::max()))));

                    // Iterate over all the expired pre-images in the collection in order to find
                    // the max RecordId.
                    while ((preImageAttributes = getNextPreImageAttributes(planExecutor)) &&
                           preImageAttributes->isExpiredPreImage(_preImageExpirationTime,
                                                                 _earliestOplogEntryTimestamp) &&
                           preImageAttributes->collectionUUID == currentCollectionUUID) {
                        lastExpiredPreimageTs = preImageAttributes->ts;
                    }

                    maxKey =
                        toRecordId(ChangeStreamPreImageId(currentCollectionUUID,
                                                          lastExpiredPreimageTs,
                                                          std::numeric_limits<int64_t>::max()));
                } else {
                    // If the expiration parameter is not set, then the last expired pre-image
                    // timestamp equals to one increment before the '_earliestOplogEntryTimestamp'.
                    maxKey = toRecordId(
                        ChangeStreamPreImageId(currentCollectionUUID,
                                               Timestamp(_earliestOplogEntryTimestamp.asULL() - 1),
                                               std::numeric_limits<int64_t>::max()));
                }
                tassert(6138300,
                        "Max key of the expired pre-image range has to be valid",
                        maxKey.isValid());
                _currentExpiredPreImageRange = std::pair(minKey, maxKey);
                return;
            }
        }

        // Set up the new collection scan to start from the 'minKey'.
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> createCollectionScan(
            boost::optional<RecordIdBound> minKey) const {
            return InternalPlanner::collectionScan(_opCtx,
                                                   _preImagesCollPtr,
                                                   PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                   InternalPlanner::Direction::FORWARD,
                                                   boost::none,
                                                   minKey);
        }

        OperationContext* _opCtx;
        const CollectionPtr* _preImagesCollPtr;
        RecordIdRange _currentExpiredPreImageRange;
        boost::optional<UUID> _previousCollectionUUID;
        const Timestamp _earliestOplogEntryTimestamp;

        // The pre-images with operation time less than or equal to the '_preImageExpirationTime'
        // are considered expired.
        const boost::optional<Date_t> _preImageExpirationTime;
    };

    ChangeStreamExpiredPreImageIterator(
        OperationContext* opCtx,
        const CollectionPtr* preImagesCollPtr,
        const Timestamp earliestOplogEntryTimestamp,
        const boost::optional<Date_t> preImageExpirationTime = boost::none)
        : _opCtx(opCtx),
          _preImagesCollPtr(preImagesCollPtr),
          _earliestOplogEntryTimestamp(earliestOplogEntryTimestamp),
          _preImageExpirationTime(preImageExpirationTime) {}

    Iterator begin() const {
        return Iterator(
            _opCtx, _preImagesCollPtr, _earliestOplogEntryTimestamp, _preImageExpirationTime);
    }

    Iterator end() const {
        return Iterator(_opCtx,
                        _preImagesCollPtr,
                        _earliestOplogEntryTimestamp,
                        _preImageExpirationTime,
                        true /*isEndIterator*/);
    }

private:
    OperationContext* _opCtx;
    const CollectionPtr* _preImagesCollPtr;
    const Timestamp _earliestOplogEntryTimestamp;
    const boost::optional<Date_t> _preImageExpirationTime;
};

void deleteExpiredChangeStreamPreImages(Client* client, Date_t currentTimeForTimeBasedExpiration) {
    const auto startTime = Date_t::now();
    auto opCtx = client->makeOperationContext();

    // Acquire intent-exclusive lock on the pre-images collection. Early exit if the collection
    // doesn't exist.
    AutoGetCollection autoColl(
        opCtx.get(), NamespaceString::kChangeStreamPreImagesNamespace, MODE_IX);
    const auto& preImagesColl = autoColl.getCollection();
    if (!preImagesColl) {
        return;
    }

    // Do not run the job on secondaries.
    if (!repl::ReplicationCoordinator::get(opCtx.get())
             ->canAcceptWritesForDatabase(opCtx.get(), NamespaceString::kAdminDb)) {
        return;
    }

    // Get the timestamp of the ealiest oplog entry.
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(client->getServiceContext())
            ->getEarliestOplogTimestamp(opCtx.get());

    const bool isBatchedRemoval = gBatchedExpiredChangeStreamPreImageRemoval.load();
    size_t numberOfRemovals = 0;

    ChangeStreamExpiredPreImageIterator expiredPreImages(
        opCtx.get(),
        &preImagesColl,
        currentEarliestOplogEntryTs,
        ::mongo::preImageRemoverInternal::getPreImageExpirationTime(
            opCtx.get(), currentTimeForTimeBasedExpiration));

    for (const auto& collectionRange : expiredPreImages) {
        writeConflictRetry(opCtx.get(),
                           "ChangeStreamExpiredPreImagesRemover",
                           NamespaceString::kChangeStreamPreImagesNamespace.ns(),
                           [&] {
                               auto params = std::make_unique<DeleteStageParams>();
                               params->isMulti = true;

                               std::unique_ptr<BatchedDeleteStageBatchParams> batchParams;
                               if (isBatchedRemoval) {
                                   batchParams = std::make_unique<BatchedDeleteStageBatchParams>();
                               }

                               auto exec = InternalPlanner::deleteWithCollectionScan(
                                   opCtx.get(),
                                   &preImagesColl,
                                   std::move(params),
                                   PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                   InternalPlanner::Direction::FORWARD,
                                   RecordIdBound(collectionRange.first),
                                   RecordIdBound(collectionRange.second),
                                   std::move(batchParams));
                               numberOfRemovals += exec->executeDelete();
                           });
    }

    if (numberOfRemovals > 0) {
        LOGV2_DEBUG(5869104,
                    3,
                    "Periodic expired pre-images removal job finished executing",
                    "numberOfRemovals"_attr = numberOfRemovals,
                    "jobDuration"_attr = (Date_t::now() - startTime).toString());
    }
}

void performExpiredChangeStreamPreImagesRemovalPass(Client* client) {
    try {
        Date_t currentTimeForTimeBasedExpiration = Date_t::now();

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
        deleteExpiredChangeStreamPreImages(client, currentTimeForTimeBasedExpiration);
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
