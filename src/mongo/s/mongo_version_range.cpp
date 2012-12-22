/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/s/mongo_version_range.h"

#include "mongo/util/stringutils.h"

namespace mongo {

    using std::string;
    using std::vector;

    bool MongoVersionRange::parseBSONArray(const BSONArray& arr,
                                           vector<MongoVersionRange>* excludes,
                                           std::string* errMsg)
    {
        string dummy;
        if (!errMsg) errMsg = &dummy;

        BSONObjIterator it(arr);

        while (it.more()) {
            MongoVersionRange range;
            if (!range.parseBSONElement(it.next(), errMsg)) return false;
            excludes->push_back(range);
        }

        return true;
    }

    BSONArray MongoVersionRange::toBSONArray(const vector<MongoVersionRange>& ranges) {

        BSONArrayBuilder barr;

        for (vector<MongoVersionRange>::const_iterator it = ranges.begin(); it != ranges.end();
                ++it)
        {
            const MongoVersionRange& range = *it;
            range.toBSONElement(&barr);
        }

        return barr.arr();
    }

    bool MongoVersionRange::parseBSONElement(const BSONElement& el, string* errMsg) {

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (el.type() == String) {
            minVersion = el.String();
            if (minVersion == "") {
                *errMsg = (string) "cannot parse single empty mongo version (" + el.toString()
                          + ")";
                return false;
            }
            return true;
        }
        else if (el.type() == Array || el.type() == Object) {

            BSONObj range = el.Obj();

            if (range.nFields() != 2) {
                *errMsg = (string) "not enough fields in mongo version range (" + el.toString()
                          + ")";
                return false;
            }

            BSONObjIterator it(range);

            BSONElement subElA = it.next();
            BSONElement subElB = it.next();

            if (subElA.type() != String || subElB.type() != String) {
                *errMsg = (string) "wrong field type for mongo version range (" + el.toString()
                          + ")";
                return false;
            }

            minVersion = subElA.String();
            maxVersion = subElB.String();

            if (minVersion == "") {
                *errMsg = (string) "cannot parse first empty mongo version (" + el.toString() + ")";
                return false;
            }

            if (maxVersion == "") {
                *errMsg = (string) "cannot parse second empty mongo version (" + el.toString()
                          + ")";
                return false;
            }

            if (versionCmp(minVersion, maxVersion) > 0) {
                string swap = minVersion;
                minVersion = maxVersion;
                maxVersion = swap;
            }

            return true;
        }
        else {
            *errMsg = (string) "wrong type for mongo version range " + el.toString();
            return false;
        }
    }

    void MongoVersionRange::toBSONElement(BSONArrayBuilder* barr) const {
        if (maxVersion == "") {
            barr->append(minVersion);
        }
        else {
            BSONArrayBuilder rangeB(barr->subarrayStart());

            rangeB.append(minVersion);
            rangeB.append(maxVersion);

            rangeB.done();
        }
    }

    bool MongoVersionRange::isInRange(const StringData& version) const {

        if (maxVersion == "") {
            // If a prefix of the version specified is excluded, the specified version is
            // excluded
            if (version.find(minVersion) == 0) return true;
        }
        else {
            // Range is inclusive, so make sure the end and beginning prefix excludes all
            // prefixed versions as above
            if (version.find(minVersion) == 0) return true;
            if (version.find(maxVersion) == 0) return true;
            if (versionCmp(minVersion, version) <= 0 && versionCmp(maxVersion, version) >= 0) {
                return true;
            }
        }

        return false;
    }

    bool isInMongoVersionRanges(const StringData& version, const vector<MongoVersionRange>& ranges)
    {
        for (vector<MongoVersionRange>::const_iterator it = ranges.begin(); it != ranges.end();
                ++it)
        {
            if (it->isInRange(version)) return true;
        }

        return false;
    }

}
