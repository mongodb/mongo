// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/cursor_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <memory.h>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

class GeoNearCursorStage final : public CursorStage {
public:
    GeoNearCursorStage(std::string_view sourceName,
                       const boost::intrusive_ptr<CatalogResourceHandle>&,
                       const boost::intrusive_ptr<ExpressionContext>&,
                       CursorType,
                       ResumeTrackingType,
                       boost::optional<FieldPath> distanceField,
                       boost::optional<FieldPath> locationField,
                       double distanceMultiplier,
                       const std::shared_ptr<CursorSharedState>&);

private:
    /**
     * Transforms 'obj' into a Document, calculating the distance.
     */
    Document transformDoc(Document&& obj) const final;

    // The output field in which to store the computed distance, if specified.
    boost::optional<FieldPath> _distanceField;

    // The output field to store the point that matched, if specified.
    boost::optional<FieldPath> _locationField;

    // A multiplicative factor applied to each distance. For example, you can use this to convert
    // radian distances into meters by multiplying by the radius of the Earth.
    double _distanceMultiplier;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
