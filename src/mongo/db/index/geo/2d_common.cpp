// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/geo/2d_common.h"

namespace mongo::index2d {
void parse2dParams(const BSONObj& infoObj, TwoDIndexingParams* out) {
    BSONObjIterator i(infoObj.getObjectField("key"));

    while (i.more()) {
        BSONElement e = i.next();
        if (e.type() == BSONType::string && IndexNames::GEO_2D == e.str()) {
            uassert(16800, "can't have 2 geo fields", out->geo.empty());
            uassert(16801, "2d has to be first in index", out->other.empty());
            out->geo = e.fieldName();
        } else {
            int order = 1;
            if (e.isNumber()) {
                order = e.safeNumberInt();
            }
            out->other.emplace_back(e.fieldName(), order);
        }
    }

    uassert(16802, "no geo field specified", out->geo.size());

    auto result = GeoHashConverter::createFromDoc(infoObj);
    uassertStatusOK(result.getStatus());
    out->geoHashConverter.reset(result.getValue().release());
}
}  // namespace mongo::index2d
