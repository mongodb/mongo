// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/util/modules.h"

namespace mongo::parsers::matcher {
Status parseGeoExpressionFromBSON(const BSONObj& obj, GeoExpression& expr);

Status parseGeoNearExpressionFromBSON(const BSONObj& obj, GeoNearExpression& expr);
}  // namespace mongo::parsers::matcher
