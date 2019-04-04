/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/s/catalog/merizo_version_range.h"

#include "merizo/util/stringutils.h"

namespace merizo {

using std::string;
using std::vector;

BSONArray MerizoVersionRange::toBSONArray(const vector<MerizoVersionRange>& ranges) {
    BSONArrayBuilder barr;

    for (vector<MerizoVersionRange>::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        const MerizoVersionRange& range = *it;
        range.toBSONElement(&barr);
    }

    return barr.arr();
}

bool MerizoVersionRange::parseBSONElement(const BSONElement& el, string* errMsg) {
    string dummy;
    if (!errMsg)
        errMsg = &dummy;

    if (el.type() == String) {
        minVersion = el.String();
        if (minVersion == "") {
            *errMsg = (string) "cannot parse single empty merizo version (" + el.toString() + ")";
            return false;
        }
        return true;
    } else if (el.type() == Array || el.type() == Object) {
        BSONObj range = el.Obj();

        if (range.nFields() != 2) {
            *errMsg = (string) "not enough fields in merizo version range (" + el.toString() + ")";
            return false;
        }

        BSONObjIterator it(range);

        BSONElement subElA = it.next();
        BSONElement subElB = it.next();

        if (subElA.type() != String || subElB.type() != String) {
            *errMsg = (string) "wrong field type for merizo version range (" + el.toString() + ")";
            return false;
        }

        minVersion = subElA.String();
        maxVersion = subElB.String();

        if (minVersion == "") {
            *errMsg = (string) "cannot parse first empty merizo version (" + el.toString() + ")";
            return false;
        }

        if (maxVersion == "") {
            *errMsg = (string) "cannot parse second empty merizo version (" + el.toString() + ")";
            return false;
        }

        if (versionCmp(minVersion, maxVersion) > 0) {
            string swap = minVersion;
            minVersion = maxVersion;
            maxVersion = swap;
        }

        return true;
    } else {
        *errMsg = (string) "wrong type for merizo version range " + el.toString();
        return false;
    }
}

void MerizoVersionRange::toBSONElement(BSONArrayBuilder* barr) const {
    if (maxVersion == "") {
        barr->append(minVersion);
    } else {
        BSONArrayBuilder rangeB(barr->subarrayStart());

        rangeB.append(minVersion);
        rangeB.append(maxVersion);

        rangeB.done();
    }
}

bool MerizoVersionRange::isInRange(StringData version) const {
    if (maxVersion == "") {
        // If a prefix of the version specified is excluded, the specified version is
        // excluded
        if (version.find(minVersion) == 0)
            return true;
    } else {
        // Range is inclusive, so make sure the end and beginning prefix excludes all
        // prefixed versions as above
        if (version.find(minVersion) == 0)
            return true;
        if (version.find(maxVersion) == 0)
            return true;
        if (versionCmp(minVersion, version) <= 0 && versionCmp(maxVersion, version) >= 0) {
            return true;
        }
    }

    return false;
}

bool isInMerizoVersionRanges(StringData version, const vector<MerizoVersionRange>& ranges) {
    for (vector<MerizoVersionRange>::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        if (it->isInRange(version))
            return true;
    }

    return false;
}
}
