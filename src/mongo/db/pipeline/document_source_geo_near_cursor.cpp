/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_geo_near_cursor.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <list>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

boost::intrusive_ptr<DocumentSourceGeoNearCursor> DocumentSourceGeoNearCursor::create(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    FieldPath distanceField,
    boost::optional<FieldPath> locationField,
    double distanceMultiplier) {
    return {new DocumentSourceGeoNearCursor(collections,
                                            std::move(exec),
                                            expCtx,
                                            std::move(distanceField),
                                            std::move(locationField),
                                            distanceMultiplier)};
}

DocumentSourceGeoNearCursor::DocumentSourceGeoNearCursor(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    FieldPath distanceField,
    boost::optional<FieldPath> locationField,
    double distanceMultiplier)
    : DocumentSourceCursor(
          collections, std::move(exec), expCtx, DocumentSourceCursor::CursorType::kRegular),
      _distanceField(std::move(distanceField)),
      _locationField(std::move(locationField)),
      _distanceMultiplier(distanceMultiplier) {
    invariant(_distanceMultiplier >= 0);
}

const char* DocumentSourceGeoNearCursor::getSourceName() const {
    return DocumentSourceGeoNearCursor::kStageName.rawData();
}

Document DocumentSourceGeoNearCursor::transformDoc(Document&& objInput) const {
    MutableDocument output(std::move(objInput));

    // Scale the distance by the requested factor.
    invariant(output.peek().metadata().hasGeoNearDistance(),
              str::stream()
                  << "Query returned a document that is unexpectedly missing the geoNear distance: "
                  << output.peek().toString());
    const auto distance = output.peek().metadata().getGeoNearDistance() * _distanceMultiplier;

    output.setNestedField(_distanceField, Value(distance));
    if (_locationField) {
        invariant(
            output.peek().metadata().hasGeoNearPoint(),
            str::stream()
                << "Query returned a document that is unexpectedly missing the geoNear point: "
                << output.peek().toString());
        output.setNestedField(*_locationField, output.peek().metadata().getGeoNearPoint());
    }

    // In a cluster, $geoNear will be merged via $sort, so add the sort key.
    if (pExpCtx->needsMerge) {
        const bool isSingleElementKey = true;
        output.metadata().setSortKey(Value(distance), isSingleElementKey);
    }

    return output.freeze();
}
}  // namespace mongo
