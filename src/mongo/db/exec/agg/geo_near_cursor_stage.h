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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/cursor_stage.h"
#include "mongo/db/exec/document_value/document.h"

#include <memory.h>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

class GeoNearCursorStage final : public CursorStage {
public:
    GeoNearCursorStage(StringData sourceName,
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
