// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/unpack_timeseries_bucket.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/snapshot.h"

#include <utility>
#include <vector>

namespace mongo {
namespace {

void transitionToOwnedObj(Document&& doc, WorkingSetMember* member) {
    member->keyData.clear();
    member->recordId = {};
    member->doc = {{}, std::move(doc)};
    member->transitionToOwnedObj();
}
}  // namespace


UnpackTimeseriesBucket::UnpackTimeseriesBucket(ExpressionContext* expCtx,
                                               WorkingSet* ws,
                                               std::unique_ptr<PlanStage> child,
                                               timeseries::BucketUnpacker bucketUnpacker)
    : PlanStage{kStageType, expCtx}, _ws{*ws}, _bucketUnpacker{std::move(bucketUnpacker)} {
    _children.emplace_back(std::move(child));
}

std::unique_ptr<PlanStageStats> UnpackTimeseriesBucket::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<UnpackTimeseriesBucketStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

PlanStage::StageState UnpackTimeseriesBucket::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (!_bucketUnpacker.hasNext()) {
        auto id = WorkingSet::INVALID_ID;
        auto status = child()->work(&id);

        if (PlanStage::ADVANCED == status) {
            auto member = _ws.get(id);

            // Make an owned copy of the bucket document if necessary. The bucket will be unwound
            // across multiple calls to 'doWork()', so we need to hold our own copy in the query
            // execution layer in case the storage engine reclaims the memory for the bucket between
            // calls to 'doWork()'.
            auto ownedBucket = member->doc.value().toBson().getOwned();
            _bucketUnpacker.reset(std::move(ownedBucket));

            auto measurement = _bucketUnpacker.getNext();
            transitionToOwnedObj(std::move(measurement), member);
            ++_specificStats.nBucketsUnpacked;

            *out = id;
        } else if (PlanStage::NEED_YIELD == status) {
            *out = id;
        }
        return status;
    }

    auto measurement = _bucketUnpacker.getNext();
    *out = _ws.allocate();
    auto member = _ws.get(*out);

    transitionToOwnedObj(std::move(measurement), member);

    return PlanStage::ADVANCED;
}
}  // namespace mongo
