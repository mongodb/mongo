/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/index/2d_common.h"

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
