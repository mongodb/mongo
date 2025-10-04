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

const char* UnpackTimeseriesBucket::kStageType = "UNPACK_BUCKET";

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
