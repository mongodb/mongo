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
    // Iterator over the expired pre-images.
    struct Iterator {
        using iterator_category = std::forward_iterator_tag;
        using difference_type = void;
        using value_type = BSONObj;
        using pointer = const BSONObj*;
        using reference = const BSONObj&;

        Iterator(OperationContext* opCtx,
                 const CollectionPtr* preImagesCollPtr,
                 Timestamp earliestOplogEntryTimestamp,
                 boost::optional<ChangeStreamPreImageId> minPreImageId = boost::none)
            : _opCtx(opCtx),
              _preImagesCollPtr(preImagesCollPtr),
              _earliestOplogEntryTimestamp(earliestOplogEntryTimestamp) {
            setupPlanExecutor(minPreImageId);
            advance();
        }

        reference operator*() const {
            return _currentPreImageObj;
        }

        pointer operator->() const {
            return &_currentPreImageObj;
        }

        Iterator& operator++() {
            advance();
            return *this;
        }

        // Both iterators are equal if they are both at the same pre-image.
        friend bool operator==(const Iterator& a, const Iterator& b) {
            return a._currentPreImageObj.woCompare(b._currentPreImageObj) == 0;
        };

        friend bool operator!=(const Iterator& a, const Iterator& b) {
            return !(a == b);
        };

        void saveState() {
            _planExecutor->saveState();
        }

        void restoreState() {
            _planExecutor->restoreState(_preImagesCollPtr);
        }

    private:
        // Scans the pre-images collection and gets the next expired pre-image or sets
        // 'currentPreImage' to BSONObj() in case there are no more expired pre-images left.
        void advance() {
            const auto getNextPreImage = [&]() -> boost::optional<ChangeStreamPreImage> {
                if (_planExecutor->getNext(&_currentPreImageObj, nullptr) == PlanExecutor::IS_EOF) {
                    _currentPreImageObj = BSONObj();
                    return boost::none;
                }

                return {ChangeStreamPreImage::parse(IDLParserErrorContext("pre-image"),
                                                    _currentPreImageObj)};
            };

            // If current collection has no more expired pre-images, fetch the first pre-image from
            // the next collection that has pre-images enabled.
            boost::optional<ChangeStreamPreImage> preImage;
            while ((preImage = getNextPreImage()) && !isExpiredPreImage(*preImage)) {
                // Set the maximum values for timestamp and apply ops fields such that we jump to
                // the next collection that has the pre-images enabled.
                preImage->getId().setTs(Timestamp::max());
                preImage->getId().setApplyOpsIndex(std::numeric_limits<int64_t>::max());
                setupPlanExecutor(preImage->getId());
            }
        }

        // Pre-image is expired if its timestamp is smaller than the 'earliestOplogEntryTimestamp'
        bool isExpiredPreImage(const ChangeStreamPreImage& preImage) const {
            return preImage.getId().getTs() < _earliestOplogEntryTimestamp;
        }

        // Set up the new collection scan to start from the 'minPreImageId'.
        void setupPlanExecutor(boost::optional<ChangeStreamPreImageId> minPreImageId) {
            const auto minRecordId =
                (minPreImageId ? boost::optional<RecordId>(record_id_helpers::keyForElem(
                                     BSON("_id" << minPreImageId->toBSON()).firstElement()))
                               : boost::none);
            _planExecutor =
                InternalPlanner::collectionScan(_opCtx,
                                                _preImagesCollPtr,
                                                PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                InternalPlanner::Direction::FORWARD,
                                                boost::none,
                                                minRecordId);
        }

        OperationContext* _opCtx;
        const CollectionPtr* _preImagesCollPtr;
        BSONObj _currentPreImageObj;
        Timestamp _earliestOplogEntryTimestamp;
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _planExecutor;
    };

    ChangeStreamExpiredPreImageIterator(OperationContext* opCtx,
                                        const CollectionPtr* preImagesCollPtr,
                                        Timestamp earliestOplogEntryTimestamp)
        : _opCtx(opCtx),
          _preImagesCollPtr(preImagesCollPtr),
          _earliestOplogEntryTimestamp(earliestOplogEntryTimestamp) {}

    Iterator begin() const {
        return Iterator(_opCtx, _preImagesCollPtr, _earliestOplogEntryTimestamp);
    }

    Iterator end() const {
        // For end iterator the collection scan 'minRecordId' has to be maximum pre-image id.
        const auto maxUUID = UUID::parse("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF").getValue();
        ChangeStreamPreImageId maxPreImageId(
            maxUUID, Timestamp::max(), std::numeric_limits<int64_t>::max());
        return Iterator(_opCtx, _preImagesCollPtr, _earliestOplogEntryTimestamp, maxPreImageId);
    }

private:
    OperationContext* _opCtx;
    const CollectionPtr* _preImagesCollPtr;
    Timestamp _earliestOplogEntryTimestamp;
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

    const bool shouldReplicateDeletes = gIsChangeStreamExpiredPreImageRemovalJobReplicating.load();
    const auto isPrimary = repl::ReplicationCoordinator::get(opCtx.get())
                               ->canAcceptWritesForDatabase(opCtx.get(), NamespaceString::kAdminDb);

    // Do not run the job on secondaries if we should explicitly replicate the deletes.
    if (!isPrimary && shouldReplicateDeletes) {
        return;
    }

    // Get the timestamp of the ealiest oplog entry.
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(client->getServiceContext())
            ->getEarliestOplogTimestamp(opCtx.get());

    // Iterate over all expired pre-images and remove them.
    size_t numberOfRemovals = 0;
    ChangeStreamExpiredPreImageIterator expiredPreImages(
        opCtx.get(), &preImagesColl, currentEarliestOplogEntryTs);
    // TODO SERVER-58693: consider adopting a recordID range-based deletion policy instead of
    // iterating.
    for (auto it = expiredPreImages.begin(); it != expiredPreImages.end(); ++it) {
        it.saveState();

        writeConflictRetry(
            opCtx.get(),
            "ChangeStreamExpiredPreImagesRemover",
            NamespaceString::kChangeStreamPreImagesNamespace.ns(),
            [&] {
                boost::optional<repl::UnreplicatedWritesBlock> unReplBlock;
                // TODO SERVER-60238: write the tests for non-replicating deletes, when pre-image
                // replication to secondaries is implemented.
                if (!shouldReplicateDeletes) {
                    unReplBlock.emplace(opCtx.get());
                }

                WriteUnitOfWork wuow(opCtx.get());
                const auto recordId =
                    record_id_helpers::keyForElem(it->getField(ChangeStreamPreImage::kIdFieldName));
                preImagesColl->deleteDocument(
                    opCtx.get(), kUninitializedStmtId, recordId, &CurOp::get(*opCtx)->debug());
                wuow.commit();
                numberOfRemovals++;
            });

        it.restoreState();
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
