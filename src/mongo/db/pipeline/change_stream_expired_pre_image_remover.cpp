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

#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

namespace mongo {

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
        struct PreImageAttributes {
            mongo::UUID collectionUUID;
            Timestamp ts;
            Date_t operationTime;
        };

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
                -> boost::optional<PreImageAttributes> {
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
                if (!isExpiredPreImage(*preImageAttributes)) {
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
                           isExpiredPreImage(*preImageAttributes) &&
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

        // Computes if the pre-image is considered expired based on the expiration parameter being
        // set.
        bool isExpiredPreImage(const PreImageAttributes& preImageAttributes) const {
            // Pre-image oplog entry is no longer present in the oplog if its timestamp is smaller
            // than the 'earliestOplogEntryTimestamp'.
            const bool preImageOplogEntryIsDeleted =
                preImageAttributes.ts < _earliestOplogEntryTimestamp;
            const auto expirationTime = _preImageExpirationTime.get_value_or(Date_t::min());

            // Pre-image is expired if its corresponding oplog entry is deleted or its operation
            // time is less than or equal to the expiration time.
            return preImageOplogEntryIsDeleted ||
                preImageAttributes.operationTime <= expirationTime;
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

void deleteExpiredChangeStreamPreImages(Client* client) {
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
    const auto isPrimary = repl::ReplicationCoordinator::get(opCtx.get())
                               ->canAcceptWritesForDatabase(opCtx.get(), NamespaceString::kAdminDb);
    if (!isPrimary) {
        return;
    }

    // Get the timestamp of the ealiest oplog entry.
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(client->getServiceContext())
            ->getEarliestOplogTimestamp(opCtx.get());

    // Iterate over all expired pre-images and remove them.
    size_t numberOfRemovals = 0;
    // TODO SERVER-58693: pass expiration duration parameter to the iterator.
    ChangeStreamExpiredPreImageIterator expiredPreImages(
        opCtx.get(), &preImagesColl, currentEarliestOplogEntryTs);

    for (const auto& collectionRange : expiredPreImages) {
        writeConflictRetry(opCtx.get(),
                           "ChangeStreamExpiredPreImagesRemover",
                           NamespaceString::kChangeStreamPreImagesNamespace.ns(),
                           [&] {
                               auto params = std::make_unique<DeleteStageParams>();
                               params->isMulti = true;

                               const auto exec = InternalPlanner::deleteWithCollectionScan(
                                   opCtx.get(),
                                   &preImagesColl,
                                   std::move(params),
                                   PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                   InternalPlanner::Direction::FORWARD,
                                   RecordIdBound(collectionRange.first),
                                   RecordIdBound(collectionRange.second));
                               numberOfRemovals += exec->executeDelete();
                           });
    }

    LOGV2(5869104,
          "Periodic expired pre-images removal job finished executing",
          "numberOfRemovals"_attr = numberOfRemovals,
          "jobDuration"_attr = (Date_t::now() - startTime).toString());
}
}  // namespace

PeriodicChangeStreamExpiredPreImagesRemover& PeriodicChangeStreamExpiredPreImagesRemover::get(
    ServiceContext* serviceContext) {
    auto& jobContainer = _serviceDecoration(serviceContext);
    jobContainer._init(serviceContext);
    return jobContainer;
}

PeriodicJobAnchor& PeriodicChangeStreamExpiredPreImagesRemover::operator*() const noexcept {
    stdx::lock_guard lk(_mutex);
    return *_anchor;
}

PeriodicJobAnchor* PeriodicChangeStreamExpiredPreImagesRemover::operator->() const noexcept {
    stdx::lock_guard lk(_mutex);
    return _anchor.get();
}

void PeriodicChangeStreamExpiredPreImagesRemover::_init(ServiceContext* serviceContext) {
    stdx::lock_guard lk(_mutex);
    if (_anchor) {
        return;
    }

    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "ChangeStreamExpiredPreImagesRemover",
        [](Client* client) {
            try {
                deleteExpiredChangeStreamPreImages(client);
            } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
                LOGV2_WARNING(5869105, "Periodic expired pre-images removal job was interrupted");
            } catch (const DBException& exception) {
                LOGV2_ERROR(5869106,
                            "Periodic expired pre-images removal job failed",
                            "reason"_attr = exception.reason());
            }
        },
        Seconds(gExpiredChangeStreamPreImageRemovalJobSleepSecs.load()));

    _anchor = std::make_shared<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));
}

}  // namespace mongo
