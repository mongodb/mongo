// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/dotted_path/dotted_path_support.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace mongo {
namespace bson {

namespace {

const BSONObj kNullObj = BSON("" << BSONNULL);
const BSONElement kNullElt = kNullObj.firstElement();

}  // namespace

BSONElement extractElementAtDottedPath(const BSONObj& obj, std::string_view path) {
    size_t dot_offset = path.find('.');

    if (dot_offset == std::string::npos) {
        return obj.getField(path);
    }

    std::string_view left = path.substr(0, dot_offset);
    std::string_view right = path.substr(dot_offset + 1);
    BSONObj sub = obj.getObjectField(left);
    return sub.isEmpty() ? BSONElement() : extractElementAtDottedPath(sub, right);
}

BSONElement extractElementAtOrArrayAlongDottedPath(const BSONObj& obj, const char*& path) {
    const char* p = strchr(path, '.');

    BSONElement sub;

    if (p) {
        sub = obj.getField(std::string_view(path, p - path));
        path = p + 1;
    } else {
        sub = obj.getField(path);
        path = path + strlen(path);
    }

    if (sub.eoo())
        return BSONElement();
    else if (sub.type() == BSONType::array || path[0] == '\0')
        return sub;
    else if (sub.type() == BSONType::object)
        return extractElementAtOrArrayAlongDottedPath(sub.embeddedObject(), path);
    else
        return BSONElement();
}

BSONObj extractElementsBasedOnTemplate(const BSONObj& obj,
                                       const BSONObj& pattern,
                                       bool useNullIfMissing) {
    // scanandorder.h can make a zillion of these, so we start the allocation very small.
    BSONObjBuilder b(32);
    BSONObjIterator i(pattern);
    while (i.more()) {
        BSONElement e = i.next();
        const auto name = e.fieldNameStringData();
        BSONElement x = extractElementAtDottedPath(obj, name);
        if (!x.eoo())
            b.appendAs(x, name);
        else if (useNullIfMissing)
            b.appendNull(name);
    }
    return b.obj();
}

BSONObj extractNullForAllFieldsBasedOnTemplate(const BSONObj& pattern) {
    BSONObjBuilder b;
    BSONObjIterator i(pattern);
    while (i.more()) {
        BSONElement e = i.next();
        b.appendNull(e.fieldNameStringData());
    }
    return b.obj();
}

int compareObjectsAccordingToSort(const BSONObj& firstObj,
                                  const BSONObj& secondObj,
                                  const BSONObj& sortKey,
                                  bool assumeDottedPaths) {
    if (firstObj.isEmpty())
        return secondObj.isEmpty() ? 0 : -1;
    if (secondObj.isEmpty())
        return 1;

    uassert(10060, "compareObjectsAccordingToSort() needs a non-empty sortKey", !sortKey.isEmpty());

    BSONObjIterator i(sortKey);
    while (1) {
        BSONElement f = i.next();
        if (f.eoo())
            return 0;

        const auto name = f.fieldNameStringData();
        BSONElement l = assumeDottedPaths ? extractElementAtDottedPath(firstObj, name)
                                          : firstObj.getField(name);
        if (l.eoo())
            l = kNullElt;
        BSONElement r = assumeDottedPaths ? extractElementAtDottedPath(secondObj, name)
                                          : secondObj.getField(name);
        if (r.eoo())
            r = kNullElt;

        int x = l.woCompare(r, false);
        if (f.number() < 0)
            x = -x;
        if (x != 0)
            return x;
    }
    return -1;
}

}  // namespace bson
}  // namespace mongo
