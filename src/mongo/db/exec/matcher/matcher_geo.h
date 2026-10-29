// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/matcher/match_details.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/index/geo/s2_common.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace exec::matcher {

bool geoContains(const GeometryContainer& queryGeom,
                 const GeoExpression::Predicate& queryPredicate,
                 bool skipValidation,
                 const BSONElement& e,
                 boost::optional<S2IndexVersion> indexVersion = boost::none);

bool geoContains(const GeometryContainer& queryGeom,
                 const GeoExpression::Predicate& queryPredicate,
                 GeometryContainer& otherContainer);

bool matchesGeoContainer(const GeoMatchExpression* expr, const GeometryContainer& input);

}  // namespace exec::matcher
}  // namespace mongo
