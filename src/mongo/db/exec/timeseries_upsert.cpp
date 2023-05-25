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

#include "mongo/db/exec/timeseries_upsert.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/update/update_util.h"

namespace mongo {

TimeseriesUpsertStage::TimeseriesUpsertStage(ExpressionContext* expCtx,
                                             TimeseriesModifyParams&& params,
                                             WorkingSet* ws,
                                             std::unique_ptr<PlanStage> child,
                                             const ScopedCollectionAcquisition& coll,
                                             BucketUnpacker bucketUnpacker,
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
bool TimeseriesUpsertStage::isEOF() {
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

    // If we don't need to return the inserted document, we're done.
    return PlanStage::IS_EOF;
}

void TimeseriesUpsertStage::_performInsert(BSONObj newDocument) {
    auto insertOp = timeseries::makeInsertsToNewBuckets({newDocument},
                                                        collection()->ns(),
                                                        *collection()->getTimeseriesOptions(),
                                                        collection()->getDefaultCollator());

    writeConflictRetry(opCtx(), "TimeseriesUpsert", collection()->ns(), [&] {
        timeseries::performAtomicWrites(opCtx(),
                                        collection(),
                                        RecordId{},
                                        boost::none,
                                        insertOp,
                                        _params.fromMigrate,
                                        _params.stmtId);
    });
}

BSONObj TimeseriesUpsertStage::_produceNewDocumentForInsert() {
    FieldRefSet immutablePaths;
    mutablebson::Document doc;

    if (_request.shouldUpsertSuppliedDocument()) {
        update::generateNewDocumentFromSuppliedDoc(opCtx(), immutablePaths, &_request, doc);
    } else {
        uassertStatusOK(_params.updateDriver->populateDocumentWithQueryFields(
            *_originalPredicate, immutablePaths, doc));

        update::generateNewDocumentFromUpdateOp(opCtx(), immutablePaths, _params.updateDriver, doc);
    }

    update::ensureIdFieldIsFirst(&doc, true);

    auto newDocument = doc.getObject();
    if (!DocumentValidationSettings::get(opCtx()).isInternalValidationDisabled()) {
        uassert(7655103,
                "Document to upsert is larger than {}"_format(BSONObjMaxUserSize),
                newDocument.objsize() <= BSONObjMaxUserSize);
    }

    return newDocument;
}

}  // namespace mongo
