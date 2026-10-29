// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_geo_near_cursor.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(geoNearCursor, DocumentSourceGeoNearCursor::id);

boost::intrusive_ptr<DocumentSourceGeoNearCursor> DocumentSourceGeoNearCursor::create(
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<FieldPath> distanceField,
    boost::optional<FieldPath> locationField,
    double distanceMultiplier) {
    return {new DocumentSourceGeoNearCursor(std::move(exec),
                                            expCtx,
                                            std::move(distanceField),
                                            std::move(locationField),
                                            distanceMultiplier)};
}

DocumentSourceGeoNearCursor::DocumentSourceGeoNearCursor(
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<FieldPath> distanceField,
    boost::optional<FieldPath> locationField,
    double distanceMultiplier)
    : DocumentSourceCursor(std::move(exec), expCtx, DocumentSourceCursor::CursorType::kRegular),
      _distanceField(std::move(distanceField)),
      _locationField(std::move(locationField)),
      _distanceMultiplier(distanceMultiplier) {
    tassert(9911901, "", _distanceMultiplier >= 0);
}

std::string_view DocumentSourceGeoNearCursor::getSourceName() const {
    return DocumentSourceGeoNearCursor::kStageName;
}
}  // namespace mongo
