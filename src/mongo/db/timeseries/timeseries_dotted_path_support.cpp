/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/timeseries/timeseries_dotted_path_support.h"

#include <string>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/ctype.h"

#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo {
namespace timeseries {
namespace dotted_path_support {

namespace {

boost::optional<std::pair<StringData, StringData>> _splitPath(StringData path) {
    size_t idx = path.find('.');
    if (idx == std::string::npos) {
        return boost::none;
    }

    StringData left = path.substr(0, idx);
    StringData next = path.substr(idx + 1, path.size());

    return std::make_pair(left, next);
}

template <typename BSONElementColl>
void _extractAllElementsAlongBucketPath(const BSONObj& obj,
                                        StringData path,
                                        BSONElementColl& elements,
                                        bool expandArrayOnTrailingField,
                                        BSONDepthIndex depth,
                                        MultikeyComponents* arrayComponents) {
    auto handleElement = [&](BSONElement e, StringData path) -> void {
        if (e.eoo()) {
            size_t idx = path.find('.');
            if (idx != std::string::npos) {
                invariant(depth != std::numeric_limits<BSONDepthIndex>::max());
                StringData left = path.substr(0, idx);
                StringData next = path.substr(idx + 1, path.size());

                BSONElement e = obj.getField(left);

                if (e.type() == Object) {
                    _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                       next,
                                                       elements,
                                                       expandArrayOnTrailingField,
                                                       depth + 1,
                                                       arrayComponents);
                } else if (e.type() == Array) {
                    bool allDigits = false;
                    if (next.size() > 0 && ctype::isDigit(next[0])) {
                        unsigned temp = 1;
                        while (temp < next.size() && ctype::isDigit(next[temp]))
                            temp++;
                        allDigits = temp == next.size() || next[temp] == '.';
                    }
                    if (allDigits) {
                        _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                           next,
                                                           elements,
                                                           expandArrayOnTrailingField,
                                                           depth + 1,
                                                           arrayComponents);
                    } else {
                        BSONObjIterator i(e.embeddedObject());
                        while (i.more()) {
                            BSONElement e2 = i.next();
                            if (e2.type() == Object || e2.type() == Array)
                                _extractAllElementsAlongBucketPath(e2.embeddedObject(),
                                                                   next,
                                                                   elements,
                                                                   expandArrayOnTrailingField,
                                                                   depth + 1,
                                                                   arrayComponents);
                        }
                        if (arrayComponents) {
                            arrayComponents->insert(depth);
                        }
                    }
                } else {
                    // do nothing: no match
                }
            }
        } else {
            if (e.type() == Array && expandArrayOnTrailingField) {
                BSONObjIterator i(e.embeddedObject());
                while (i.more()) {
                    elements.insert(i.next());
                }
                if (arrayComponents) {
                    arrayComponents->insert(depth);
                }
            } else {
                elements.insert(e);
            }
        }
    };

    switch (depth) {
        case 0:
        case 1: {
            if (auto res = _splitPath(path)) {
                auto& [left, next] = *res;
                BSONElement e = obj.getField(left);
                if (e.type() == Object && (depth > 0 || left == timeseries::kBucketDataFieldName)) {
                    _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                       next,
                                                       elements,
                                                       expandArrayOnTrailingField,
                                                       depth + 1,
                                                       arrayComponents);
                }
            } else {
                BSONElement e = obj.getField(path);
                if (Object == e.type()) {
                    _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                       StringData(),
                                                       elements,
                                                       expandArrayOnTrailingField,
                                                       depth + 1,
                                                       arrayComponents);
                }
            }
            break;
        }
        case 2: {
            // Unbucketing magic happens here.
            for (const BSONElement e : obj) {
                std::string subPath = e.fieldName();
                if (!path.empty()) {
                    subPath.append("." + path);
                }
                BSONElement sub = obj.getField(subPath);
                handleElement(sub, subPath);
            }
            break;
        }
        default: {
            BSONElement e = obj.getField(path);
            handleElement(e, path);
            break;
        }
    }
}

}  // namespace

void extractAllElementsAlongBucketPath(const BSONObj& obj,
                                       StringData path,
                                       BSONElementSet& elements,
                                       bool expandArrayOnTrailingField,
                                       MultikeyComponents* arrayComponents) {
    constexpr BSONDepthIndex initialDepth = 0;
    _extractAllElementsAlongBucketPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

void extractAllElementsAlongBucketPath(const BSONObj& obj,
                                       StringData path,
                                       BSONElementMultiSet& elements,
                                       bool expandArrayOnTrailingField,
                                       MultikeyComponents* arrayComponents) {
    constexpr BSONDepthIndex initialDepth = 0;
    _extractAllElementsAlongBucketPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

}  // namespace dotted_path_support
}  // namespace timeseries
}  // namespace mongo
