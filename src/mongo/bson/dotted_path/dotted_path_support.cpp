/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/dotted_path/dotted_path_support.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>

namespace mongo {
namespace bson {

namespace {

const BSONObj kNullObj = BSON("" << BSONNULL);
const BSONElement kNullElt = kNullObj.firstElement();

}  // namespace

BSONElement extractElementAtDottedPath(const BSONObj& obj, StringData path) {
    BSONElement e = obj.getField(path);
    if (e.eoo()) {
        size_t dot_offset = path.find('.');
        if (dot_offset != std::string::npos) {
            StringData left = path.substr(0, dot_offset);
            StringData right = path.substr(dot_offset + 1);
            BSONObj sub = obj.getObjectField(left);
            return sub.isEmpty() ? BSONElement() : extractElementAtDottedPath(sub, right);
        }
    }

    return e;
}

BSONElement extractElementAtOrArrayAlongDottedPath(const BSONObj& obj, const char*& path) {
    const char* p = strchr(path, '.');

    BSONElement sub;

    if (p) {
        sub = obj.getField(StringData(path, p - path));
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
