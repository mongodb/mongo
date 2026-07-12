// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/query_settings_debug_shape_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_internal_query_settings_debug_shape.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalQuerySettingsDebugShapeToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* ptr = dynamic_cast<DocumentSourceInternalQuerySettingsDebugShape*>(documentSource.get());
    tassert(12915502, "expected 'DocumentSourceInternalQuerySettingsDebugShape' type", ptr);
    return make_intrusive<exec::agg::QuerySettingsDebugShapeStage>(
        DocumentSourceInternalQuerySettingsDebugShape::kStageName, ptr->getExpCtx());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(querySettingsDebugShapeStage,
                           DocumentSourceInternalQuerySettingsDebugShape::id,
                           documentSourceInternalQuerySettingsDebugShapeToStageFn);

namespace {
BSONObj createDebugQueryShape(OperationContext* opCtx,
                              const BSONObj& representativeQuery,
                              const boost::optional<TenantId>& tenantId) {
    // Get the serialized query shape by creating the representative information for the given
    // representative query.
    const auto representativeInfo =
        query_settings::createRepresentativeInfo(opCtx, representativeQuery, tenantId);
    return representativeInfo.serializedQueryShape;
}
}  // namespace

QuerySettingsDebugShapeStage::QuerySettingsDebugShapeStage(
    std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : Stage(stageName, pExpCtx) {}

GetNextResult QuerySettingsDebugShapeStage::doGetNext() {
    auto next = pSource->getNext();
    if (!next.isAdvanced()) {
        return next;
    }

    auto doc = next.getDocument();
    auto representativeQuery = doc["representativeQuery"];
    if (!representativeQuery.isObject()) {
        return doc;
    }

    auto* opCtx = getContext()->getOperationContext();
    auto tenantId = getContext()->getNamespaceString().tenantId();
    try {
        MutableDocument out(std::move(doc));
        out[kDebugQueryShapeFieldName] = Value(
            createDebugQueryShape(opCtx, representativeQuery.getDocument().toBson(), tenantId));
        return out.freeze();
    } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
        uasserted(ErrorCodes::BSONObjectTooLarge,
                  str::stream() << "query settings object exceeds " << BSONObjMaxInternalSize
                                << " bytes; consider not setting 'showDebugQueryShape' to true");
    }
}

}  // namespace exec::agg
}  // namespace mongo
