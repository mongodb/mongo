/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/bson/dotted_path_support.h"

#include <cctype>
#include <string>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace dotted_path_support {

namespace {

const BSONObj kNullObj = BSON("" << BSONNULL);
const BSONElement kNullElt = kNullObj.firstElement();

template <typename BSONElementColl>
void _extractAllElementsAlongPath(const BSONObj& obj,
                                  StringData path,
                                  BSONElementColl& elements,
                                  bool expandArrayOnTrailingField,
                                  size_t depth,
                                  std::set<size_t>* arrayComponents) {
    BSONElement e = obj.getField(path);

    if (e.eoo()) {
        size_t idx = path.find('.');
        if (idx != std::string::npos) {
            StringData left = path.substr(0, idx);
            StringData next = path.substr(idx + 1, path.size());

            BSONElement e = obj.getField(left);

            if (e.type() == Object) {
                _extractAllElementsAlongPath(e.embeddedObject(),
                                             next,
                                             elements,
                                             expandArrayOnTrailingField,
                                             depth + 1,
                                             arrayComponents);
            } else if (e.type() == Array) {
                bool allDigits = false;
                if (next.size() > 0 && std::isdigit(next[0])) {
                    unsigned temp = 1;
                    while (temp < next.size() && std::isdigit(next[temp]))
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
                    size_t nArrElems = 0;
                    BSONObjIterator i(e.embeddedObject());
                    while (i.more()) {
                        BSONElement e2 = i.next();
                        if (e2.type() == Object || e2.type() == Array)
                            _extractAllElementsAlongPath(e2.embeddedObject(),
                                                         next,
                                                         elements,
                                                         expandArrayOnTrailingField,
                                                         depth + 1,
                                                         arrayComponents);
                        ++nArrElems;
                    }
                    if (arrayComponents && nArrElems > 1) {
                        arrayComponents->insert(depth);
                    }
                }
            } else {
                // do nothing: no match
            }
        }
    } else {
        if (e.type() == Array && expandArrayOnTrailingField) {
            size_t nArrElems = 0;
            BSONObjIterator i(e.embeddedObject());
            while (i.more()) {
                elements.insert(i.next());
                ++nArrElems;
            }
            if (arrayComponents && nArrElems > 1) {
                arrayComponents->insert(depth);
            }
        } else {
            elements.insert(e);
        }
    }
}

}  // namespace

BSONElement extractElementAtPath(const BSONObj& obj, StringData path) {
    BSONElement e = obj.getField(path);
    if (e.eoo()) {
        size_t dot_offset = path.find('.');
        if (dot_offset != std::string::npos) {
            StringData left = path.substr(0, dot_offset);
            StringData right = path.substr(dot_offset + 1);
            BSONObj sub = obj.getObjectField(left);
            return sub.isEmpty() ? BSONElement() : extractElementAtPath(sub, right);
        }
    }

    return e;
}

BSONElement extractElementAtPathOrArrayAlongPath(const BSONObj& obj, const char*& path) {
    const char* p = strchr(path, '.');

    BSONElement sub;

    if (p) {
        sub = obj.getField(std::string(path, p - path));
        path = p + 1;
    } else {
        sub = obj.getField(path);
        path = path + strlen(path);
    }

    if (sub.eoo())
        return BSONElement();
    else if (sub.type() == Array || path[0] == '\0')
        return sub;
    else if (sub.type() == Object)
        return extractElementAtPathOrArrayAlongPath(sub.embeddedObject(), path);
    else
        return BSONElement();
}

void extractAllElementsAlongPath(const BSONObj& obj,
                                 StringData path,
                                 BSONElementSet& elements,
                                 bool expandArrayOnTrailingField,
                                 std::set<size_t>* arrayComponents) {
    const size_t initialDepth = 0;
    _extractAllElementsAlongPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

void extractAllElementsAlongPath(const BSONObj& obj,
                                 StringData path,
                                 BSONElementMSet& elements,
                                 bool expandArrayOnTrailingField,
                                 std::set<size_t>* arrayComponents) {
    const size_t initialDepth = 0;
    _extractAllElementsAlongPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

BSONObj extractElementsBasedOnTemplate(const BSONObj& obj,
                                       const BSONObj& pattern,
                                       bool useNullIfMissing) {
    // scanandorder.h can make a zillion of these, so we start the allocation very small.
    BSONObjBuilder b(32);
    BSONObjIterator i(pattern);
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;
        BSONElement x = extractElementAtPath(obj, e.fieldName());
        if (!x.eoo())
            b.appendAs(x, e.fieldName());
        else if (useNullIfMissing)
            b.appendNull(e.fieldName());
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

        BSONElement l = assumeDottedPaths ? extractElementAtPath(firstObj, f.fieldName())
                                          : firstObj.getField(f.fieldName());
        if (l.eoo())
            l = kNullElt;
        BSONElement r = assumeDottedPaths ? extractElementAtPath(secondObj, f.fieldName())
                                          : secondObj.getField(f.fieldName());
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

}  // namespace dotted_path_support
}  // namespace mongo
