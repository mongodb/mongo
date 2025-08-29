/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "src/mongo/db/exec/agg/internal_apply_oplog_update_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalApplyOplogUpdateGroupToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto ds = boost::dynamic_pointer_cast<DocumentSourceInternalApplyOplogUpdate>(documentSource);

    tassert(10979800, "expected 'DocumentSourceInternalApplyOplogUpdate' type", ds);

    return make_intrusive<exec::agg::InternalApplyOplogUpdateStage>(
        ds->kStageName, ds->getExpCtx(), ds->_oplogUpdate);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalApplyOplogUpdateStage,
                           DocumentSourceInternalApplyOplogUpdate::id,
                           documentSourceInternalApplyOplogUpdateGroupToStageFn)

InternalApplyOplogUpdateStage::InternalApplyOplogUpdateStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const BSONObj& oplogUpdate)
    : Stage(stageName, pExpCtx), _updateDriver(pExpCtx) {
    // Parse the raw oplog update description.
    const auto updateMod = write_ops::UpdateModification::parseFromOplogEntry(
        oplogUpdate, {true /* mustCheckExistenceForInsertOperations */});

    // UpdateDriver only expects to apply a diff in the context of oplog application.
    _updateDriver.setFromOplogApplication(true);
    _updateDriver.parse(updateMod, {});
}

GetNextResult InternalApplyOplogUpdateStage::doGetNext() {
    auto next = pSource->getNext();
    if (!next.isAdvanced()) {
        return next;
    }

    // Use _updateDriver to apply the update to the document.
    mutablebson::Document doc(next.getDocument().toBson());
    uassertStatusOK(_updateDriver.update(pExpCtx->getOperationContext(),
                                         StringData(),
                                         &doc,
                                         false /* validateForStorage */,
                                         FieldRefSet(),
                                         false /* isInsert */));

    return Document(doc.getObject());
}

}  // namespace exec::agg
}  // namespace mongo
