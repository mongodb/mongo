// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/bson/multikey_dotted_path_support.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace mongo {
namespace multikey_dotted_path_support {

namespace {

const BSONObj kNullObj = BSON("" << BSONNULL);
const BSONElement kNullElt = kNullObj.firstElement();

template <typename BSONElementColl>
void _extractAllElementsAlongPath(const BSONObj& obj,
                                  std::string_view path,
                                  BSONElementColl& elements,
                                  bool expandArrayOnTrailingField,
                                  BSONDepthIndex depth,
                                  MultikeyComponents* arrayComponents) {
    size_t idx = path.find('.');
    if (idx != std::string::npos) {
        tassert(
            11177100,
            fmt::format(
                "Exceeded max depth while processing dotted path '{}'. Attempted depth={} exceeds "
                "BSONDepthIndex numeric limit={}",
                path,
                depth,
                std::numeric_limits<BSONDepthIndex>::max()),
            depth != std::numeric_limits<BSONDepthIndex>::max());
        std::string_view left = path.substr(0, idx);
        std::string_view next = path.substr(idx + 1, path.size());

        BSONElement e = obj.getField(left);

        if (e.type() == BSONType::object) {
            _extractAllElementsAlongPath(e.embeddedObject(),
                                         next,
                                         elements,
                                         expandArrayOnTrailingField,
                                         depth + 1,
                                         arrayComponents);
        } else if (e.type() == BSONType::array) {
            bool allDigits = false;
            if (next.size() > 0 && ctype::isDigit(next[0])) {
                unsigned temp = 1;
                while (temp < next.size() && ctype::isDigit(next[temp]))
                    temp++;
                allDigits = temp == next.size() || next[temp] == '.';
            }
            if (allDigits) {
                _extractAllElementsAlongPath(e.embeddedObject(),
                                             next,
                                             elements,
                                             expandArrayOnTrailingField,
                                             depth + 1,
                                             arrayComponents);
            } else {
                BSONObjIterator i(e.embeddedObject());
                while (i.more()) {
                    BSONElement e2 = i.next();
                    if (e2.type() == BSONType::object || e2.type() == BSONType::array)
                        _extractAllElementsAlongPath(e2.embeddedObject(),
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
    } else {
        BSONElement e = obj.getField(path);

        if (e.ok()) {
            if (e.type() == BSONType::array && expandArrayOnTrailingField) {
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
    }
}

}  // namespace

void extractAllElementsAlongPath(const BSONObj& obj,
                                 std::string_view path,
                                 BSONElementSet& elements,
                                 bool expandArrayOnTrailingField,
                                 MultikeyComponents* arrayComponents) {
    const BSONDepthIndex initialDepth = 0;
    _extractAllElementsAlongPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

void extractAllElementsAlongPath(const BSONObj& obj,
                                 std::string_view path,
                                 BSONElementMultiSet& elements,
                                 bool expandArrayOnTrailingField,
                                 MultikeyComponents* arrayComponents) {
    const BSONDepthIndex initialDepth = 0;
    _extractAllElementsAlongPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

namespace {
/**
 * Recursive helper for extractAllElementsAlongPathLegacy.
 * This is the implementation as it existed before SERVER-76875.
 */
void _extractAllElementsAlongPathLegacy(const BSONObj& obj,
                                        std::string_view path,
                                        BSONElementSet& elements,
                                        bool expandArrayOnTrailingField,
                                        BSONDepthIndex depth,
                                        MultikeyComponents* arrayComponents) {
    BSONElement e = obj.getField(path);

    if (e.eoo()) {
        size_t idx = path.find('.');
        if (idx != std::string::npos) {
            invariant(depth != std::numeric_limits<BSONDepthIndex>::max());
            std::string_view left = path.substr(0, idx);
            std::string_view next = path.substr(idx + 1, path.size());

            BSONElement e = obj.getField(left);

            if (e.type() == BSONType::object) {
                _extractAllElementsAlongPathLegacy(e.embeddedObject(),
                                                   next,
                                                   elements,
                                                   expandArrayOnTrailingField,
                                                   depth + 1,
                                                   arrayComponents);
            } else if (e.type() == BSONType::array) {
                bool allDigits = false;
                if (next.size() > 0 && ctype::isDigit(next[0])) {
                    unsigned temp = 1;
                    while (temp < next.size() && ctype::isDigit(next[temp]))
                        temp++;
                    allDigits = temp == next.size() || next[temp] == '.';
                }
                if (allDigits) {
                    _extractAllElementsAlongPathLegacy(e.embeddedObject(),
                                                       next,
                                                       elements,
                                                       expandArrayOnTrailingField,
                                                       depth + 1,
                                                       arrayComponents);
                } else {
                    BSONObjIterator i(e.embeddedObject());
                    while (i.more()) {
                        BSONElement e2 = i.next();
                        if (e2.type() == BSONType::object || e2.type() == BSONType::array)
                            _extractAllElementsAlongPathLegacy(e2.embeddedObject(),
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
        if (e.type() == BSONType::array && expandArrayOnTrailingField) {
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
}
}  // namespace

void extractAllElementsAlongPathLegacy_forValidationOnly(const BSONObj& obj,
                                                         std::string_view path,
                                                         BSONElementSet& elements,
                                                         bool expandArrayOnTrailingField,
                                                         MultikeyComponents* arrayComponents) {
    BSONDepthIndex depth = 0;
    _extractAllElementsAlongPathLegacy(
        obj, path, elements, expandArrayOnTrailingField, depth, arrayComponents);
}

}  // namespace multikey_dotted_path_support
}  // namespace mongo
