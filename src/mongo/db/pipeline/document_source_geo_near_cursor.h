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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"

#include <memory>

#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Like DocumentSourceCursor, this stage returns Documents from BSONObjs produced by a PlanExecutor,
 * but does extra work to compute distances to satisfy a $near or $nearSphere query.
 */
class DocumentSourceGeoNearCursor final : public DocumentSourceCursor {
public:
    /**
     * The name of this stage.
     */
    static constexpr StringData kStageName = "$geoNearCursor"_sd;

    /**
     * Create a new DocumentSourceGeoNearCursor. If specified, 'distanceMultiplier' must be
     * nonnegative.
     */
    static boost::intrusive_ptr<DocumentSourceGeoNearCursor> create(
        const MultipleCollectionAccessor&,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>,
        const boost::intrusive_ptr<DocumentSourceCursor::CatalogResourceHandle>&,
        const boost::intrusive_ptr<ExpressionContext>&,
        boost::optional<FieldPath> distanceField,
        boost::optional<FieldPath> locationField = boost::none,
        double distanceMultiplier = 1.0);

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const final {
        return id;
    }

private:
    DocumentSourceGeoNearCursor(
        const MultipleCollectionAccessor&,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>,
        const boost::intrusive_ptr<DocumentSourceCursor::CatalogResourceHandle>&,
        const boost::intrusive_ptr<ExpressionContext>&,
        boost::optional<FieldPath> distanceField,
        boost::optional<FieldPath> locationField,
        double distanceMultiplier);

    ~DocumentSourceGeoNearCursor() override = default;

    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceGeoNearCursorToStageFn(
        const boost::intrusive_ptr<DocumentSource>& source);

    // The output field in which to store the computed distance, if specified.
    boost::optional<FieldPath> _distanceField;
    // The output field to store the point that matched, if specified.
    boost::optional<FieldPath> _locationField;
    // A multiplicative factor applied to each distance. For example, you can use this to convert
    // radian distances into meters by multiplying by the radius of the Earth.
    double _distanceMultiplier = 1.0;
};
}  // namespace mongo
