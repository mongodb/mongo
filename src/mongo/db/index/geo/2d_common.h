// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/geo/hash.h"
#include "mongo/db/index_names.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {
struct TwoDIndexingParams {
    std::string geo;
    std::vector<std::pair<std::string, int>> other;
    std::shared_ptr<GeoHashConverter> geoHashConverter;
};

namespace index2d {
void parse2dParams(const BSONObj& infoObj, TwoDIndexingParams* out);
}  // namespace index2d
}  // namespace mongo
