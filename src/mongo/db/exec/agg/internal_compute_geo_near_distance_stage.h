// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class InternalGeoNearDistanceStage final : public Stage {
public:
    InternalGeoNearDistanceStage(std::string_view stageName,
                                 const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                 std::string key,
                                 std::unique_ptr<PointWithCRS> centroid,
                                 const BSONObj& coords,
                                 FieldPath distanceField,
                                 double distanceMultiplier);

private:
    GetNextResult doGetNext() override;

    std::string _key;
    std::unique_ptr<PointWithCRS> _centroid;
    BSONObj _coords;  // "near" option
    FieldPath _distanceField;
    double _distanceMultiplier;
};

}  // namespace mongo::exec::agg
