// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_apply_oplog_update_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

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
    std::string_view stageName,
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
                                         std::string_view(),
                                         &doc,
                                         true /* validateForStorage */,
                                         FieldRefSet(),
                                         false /* isInsert */));

    return Document(doc.getObject());
}

}  // namespace exec::agg
}  // namespace mongo
