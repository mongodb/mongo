/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/timeseries_upsert.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update/update_util.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace {

const char idFieldName[] = "_id";
const mongo::FieldRef idFieldRef(idFieldName);

}  // namespace


namespace mongo {

TimeseriesUpsertStage::TimeseriesUpsertStage(ExpressionContext* expCtx,
                                             TimeseriesModifyParams&& params,
                                             WorkingSet* ws,
                                             std::unique_ptr<PlanStage> child,
                                             CollectionAcquisition coll,
                                             timeseries::BucketUnpacker bucketUnpacker,
                                             std::unique_ptr<MatchExpression> residualPredicate,
                                             std::unique_ptr<MatchExpression> originalPredicate,
                                             const UpdateRequest& request)
    : TimeseriesModifyStage(expCtx,
                            std::move(params),
                            ws,
                            std::move(child),
                            coll,
                            std::move(bucketUnpacker),
                            std::move(residualPredicate),
                            std::move(originalPredicate)),
      _request(request) {
    // We should never create this stage for a non-upsert request.
    tassert(7655100, "request must be an upsert", _params.isUpdate && _request.isUpsert());
};

// We're done when updating is finished and we have either matched or inserted.
bool TimeseriesUpsertStage::isEOF() const {
    return TimeseriesModifyStage::isEOF() &&
        (_specificStats.nMeasurementsMatched > 0 || _specificStats.nMeasurementsUpserted > 0);
}

PlanStage::StageState TimeseriesUpsertStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return StageState::IS_EOF;
    }

    // First, attempt to perform the update on a matching document.
    auto updateState = TimeseriesModifyStage::doWork(out);

    //  If the update returned anything other than EOF, just forward it along. There's a chance we
    //  still may find a document to update and will not have to insert anything. If it did return
    //  EOF and we do not need to insert a new document, return EOF immediately here.
    if (updateState != PlanStage::IS_EOF || isEOF()) {
        return updateState;
    }

    // Since this is an insert, we will be logging it as such in the oplog. We don't need the
    // driver's help to build the oplog record. We also set the 'nUpserted' stats counter here.
    _params.updateDriver->setLogOp(false);
    _specificStats.nMeasurementsUpserted = 1;

    // Generate the new document to be inserted.
    _specificStats.objInserted = _produceNewDocumentForInsert();

    // If this is an explain, skip performing the actual insert.
    if (!_params.isExplain) {
        _performInsert(_specificStats.objInserted);
    }

    // We should always be EOF at this point.
    tassert(7655101, "must be at EOF if we performed an upsert", isEOF());

    if (!_params.returnNew) {
        // If we don't need to return the inserted document, we're done.
        return PlanStage::IS_EOF;
    }

    // If we want to return the document we just inserted, create it as a WorkingSetMember.
    _prepareToReturnMeasurement(*out, _specificStats.objInserted);
    return PlanStage::ADVANCED;
}

void TimeseriesUpsertStage::_performInsert(BSONObj newMeasurement) {
    if (_isUserInitiatedUpdate) {
        const auto& acq = collectionAcquisition();
        if (const auto& collDesc = acq.getShardingDescription(); collDesc.isSharded()) {
            auto newBucket =
                timeseries::write_ops::makeBucketDocument({newMeasurement},
                                                          acq.nss(),
                                                          collectionPtr()->uuid(),
                                                          *collectionPtr()->getTimeseriesOptions(),
                                                          collectionPtr()->getDefaultCollator());

            //  The shard key fields may not have arrays at any point along their paths.
            update::assertPathsNotArray(mutablebson::Document{newBucket},
                                        collDesc.getKeyPatternFields());

            const auto& collFilter = acq.getShardingFilter();
            invariant(collFilter);

            auto newShardKey = collDesc.getShardKeyPattern().extractShardKeyFromDoc(newBucket);
            if (!collFilter->keyBelongsToMe(newShardKey)) {
                // An attempt to upsert a document with a shard key value that belongs on
                // another shard must either be a retryable write or inside a transaction. An
                // upsert without a transaction number is legal if
                // gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi is enabled because
                // mongos will be able to start an internal transaction to handle the
                // wouldChangeOwningShard error thrown below.
                if (!feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
                        VersionContext::getDecoration(opCtx()),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    uassert(ErrorCodes::IllegalOperation,
                            "The upsert document could not be inserted onto the shard targeted "
                            "by the query, since its shard key belongs on a different shard. "
                            "Cross-shard upserts are only allowed when running in a "
                            "transaction or with retryWrites: true.",
                            opCtx()->getTxnNumber());
                }
                uasserted(WouldChangeOwningShardInfo(_originalPredicate->serialize(),
                                                     newBucket,
                                                     true,  // upsert
                                                     acq.nss(),
                                                     acq.uuid(),
                                                     newMeasurement),
                          "The document we are inserting belongs on a different shard");
            }
        }
    }
    writeConflictRetry(opCtx(), "TimeseriesUpsert", collectionPtr()->ns(), [&] {
        timeseries::performAtomicWritesForUpdate(opCtx(),
                                                 collectionPtr(),
                                                 RecordId{},
                                                 boost::none,
                                                 {newMeasurement},
                                                 *_sideBucketCatalog,
                                                 _params.fromMigrate,
                                                 _params.stmtId,
                                                 &_insertedBucketIds,
                                                 /*currentMinTime=*/
                                                 boost::none);
    });
}


BSONObj TimeseriesUpsertStage::_produceNewDocumentForInsert() {
    // Initialize immutable paths based on the shard key field(s).
    _getImmutablePaths();

    mutablebson::Document doc;

    if (_request.shouldUpsertSuppliedDocument()) {
        update::generateNewDocumentFromSuppliedDoc(opCtx(), _immutablePaths, &_request, doc);
    } else {
        // When populating the document from the query for replacement updates, we should include
        // the _id field. However, we don't want to block _id from being set/updated, so only
        // include it in 'immutablePaths' for this step.
        _immutablePaths.emplace_back(std::make_unique<FieldRef>(idFieldName));
        uassertStatusOK(_params.updateDriver->populateDocumentWithQueryFields(
            *_originalPredicate, _immutablePaths, doc));
        _immutablePaths.pop_back();

        update::generateNewDocumentFromUpdateOp(
            opCtx(), _immutablePaths, _params.updateDriver, doc);
    }

    update::ensureIdFieldIsFirst(&doc, true);

    auto newDocument = doc.getObject();
    if (!DocumentValidationSettings::get(opCtx()).isInternalValidationDisabled()) {
        uassert(7655103,
                fmt::format("Document to upsert is larger than {}", BSONObjMaxUserSize),
                newDocument.objsize() <= BSONObjMaxUserSize);
    }

    return newDocument;
}

}  // namespace mongo
