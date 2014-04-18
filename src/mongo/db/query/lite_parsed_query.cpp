/**
 *    Copyright 2013 10gen Inc.
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

#include "mongo/db/query/lite_parsed_query.h"

#include <cmath>

#include "mongo/db/dbmessage.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    const string LiteParsedQuery::cmdOptionMaxTimeMS("maxTimeMS");
    const string LiteParsedQuery::queryOptionMaxTimeMS("$maxTimeMS");

    const string LiteParsedQuery::metaTextScore("textScore");
    const string LiteParsedQuery::metaGeoNearDistance("geoNearDistance");
    const string LiteParsedQuery::metaGeoNearPoint("geoNearPoint");
    const string LiteParsedQuery::metaDiskLoc("diskloc");
    const string LiteParsedQuery::metaIndexKey("indexKey");

    // static
    Status LiteParsedQuery::make(const QueryMessage& qm, LiteParsedQuery** out) {
        auto_ptr<LiteParsedQuery> pq(new LiteParsedQuery());

        Status status = pq->init(qm.ns, qm.ntoskip, qm.ntoreturn, qm.queryOptions, qm.query,
                                 qm.fields, true);
        if (status.isOK()) { *out = pq.release(); }
        return status;
    }

    // static
    Status LiteParsedQuery::make(const string& ns, int ntoskip, int ntoreturn, int queryOptions,
                                 const BSONObj& query, const BSONObj& proj, const BSONObj& sort,
                                 const BSONObj& hint,
                                 const BSONObj& minObj, const BSONObj& maxObj,
                                 bool snapshot,
                                 bool explain,
                                 LiteParsedQuery** out) {
        auto_ptr<LiteParsedQuery> pq(new LiteParsedQuery());
        pq->_sort = sort;
        pq->_hint = hint;
        pq->_min = minObj;
        pq->_max = maxObj;
        pq->_snapshot = snapshot;
        pq->_explain = explain;

        Status status = pq->init(ns, ntoskip, ntoreturn, queryOptions, query, proj, false);
        if (status.isOK()) { *out = pq.release(); }
        return status;
    }

    // static
    StatusWith<int> LiteParsedQuery::parseMaxTimeMSCommand(const BSONObj& cmdObj) {
        return parseMaxTimeMS(cmdObj[cmdOptionMaxTimeMS]);
    }

    // static
    StatusWith<int> LiteParsedQuery::parseMaxTimeMSQuery(const BSONObj& queryObj) {
        return parseMaxTimeMS(queryObj[queryOptionMaxTimeMS]);
    }

    // static
    StatusWith<int> LiteParsedQuery::parseMaxTimeMS(const BSONElement& maxTimeMSElt) {
        if (!maxTimeMSElt.eoo() && !maxTimeMSElt.isNumber()) {
            return StatusWith<int>(ErrorCodes::BadValue,
                                   (StringBuilder()
                                       << maxTimeMSElt.fieldNameStringData()
                                       << " must be a number").str());
        }
        long long maxTimeMSLongLong = maxTimeMSElt.safeNumberLong(); // returns 0 on EOO
        if (maxTimeMSLongLong < 0 || maxTimeMSLongLong > INT_MAX) {
            return StatusWith<int>(ErrorCodes::BadValue,
                                   (StringBuilder()
                                       << maxTimeMSElt.fieldNameStringData()
                                       << " is out of range").str());
        }
        double maxTimeMSDouble = maxTimeMSElt.numberDouble();
        if (maxTimeMSElt.type() == mongo::NumberDouble
            && floor(maxTimeMSDouble) != maxTimeMSDouble) {
            return StatusWith<int>(ErrorCodes::BadValue,
                                   (StringBuilder()
                                       << maxTimeMSElt.fieldNameStringData()
                                       << " has non-integral value").str());
        }
        return StatusWith<int>(static_cast<int>(maxTimeMSLongLong));
    }

    // static
    bool LiteParsedQuery::isTextScoreMeta(BSONElement elt) {
        // elt must be foo: {$meta: "textScore"}
        if (mongo::Object != elt.type()) {
            return false;
        }
        BSONObj metaObj = elt.Obj();
        BSONObjIterator metaIt(metaObj);
        // must have exactly 1 element
        if (!metaIt.more()) {
            return false;
        }
        BSONElement metaElt = metaIt.next();
        if (!mongoutils::str::equals("$meta", metaElt.fieldName())) {
            return false;
        }
        if (mongo::String != metaElt.type()) {
            return false;
        }
        if (LiteParsedQuery::metaTextScore != metaElt.valuestr()) {
            return false;
        }
        // must have exactly 1 element
        if (metaIt.more()) {
            return false;
        }
        return true;
    }

    // static
    bool LiteParsedQuery::isDiskLocMeta(BSONElement elt) {
        // elt must be foo: {$meta: "diskloc"}
        if (mongo::Object != elt.type()) {
            return false;
        }
        BSONObj metaObj = elt.Obj();
        BSONObjIterator metaIt(metaObj);
        // must have exactly 1 element
        if (!metaIt.more()) {
            return false;
        }
        BSONElement metaElt = metaIt.next();
        if (!mongoutils::str::equals("$meta", metaElt.fieldName())) {
            return false;
        }
        if (mongo::String != metaElt.type()) {
            return false;
        }
        if (LiteParsedQuery::metaDiskLoc != metaElt.valuestr()) {
            return false;
        }
        // must have exactly 1 element
        if (metaIt.more()) {
            return false;
        }
        return true;
    }

    // static
    bool LiteParsedQuery::isValidSortOrder(const BSONObj& sortObj) {
        BSONObjIterator i(sortObj);
        while (i.more()) {
            BSONElement e = i.next();
            // fieldNameSize() includes NULL terminator. For empty field name,
            // we should be checking for 1 instead of 0.
            if (1 == e.fieldNameSize()) {
                return false;
            }
            if (isTextScoreMeta(e)) {
                continue;
            }
            long long n = e.safeNumberLong();
            if (!(e.isNumber() && (n == -1LL || n == 1LL))) {
                return false;
            }
        }
        return true;
    }

    // static
    bool LiteParsedQuery::isQueryIsolated(const BSONObj& query) {
        BSONObjIterator iter(query);
        while (iter.more()) {
            BSONElement elt = iter.next();
            if (str::equals(elt.fieldName(), "$isolated") && elt.trueValue())
                return true;
            if (str::equals(elt.fieldName(), "$atomic") && elt.trueValue())
                return true;
        }
        return false;
    }

    // static
    BSONObj LiteParsedQuery::normalizeSortOrder(const BSONObj& sortObj) {
        BSONObjBuilder b;
        BSONObjIterator i(sortObj);
        while (i.more()) {
            BSONElement e = i.next();
            if (isTextScoreMeta(e)) {
                b.append(e);
                continue;
            }
            long long n = e.safeNumberLong();
            int sortOrder = n >= 0 ? 1 : -1;
            b.append(e.fieldName(), sortOrder);
        }
        return b.obj();
    }

    LiteParsedQuery::LiteParsedQuery() : _wantMore(true), _explain(false), _snapshot(false),
                                         _returnKey(false), _showDiskLoc(false), _maxScan(0),
                                         _maxTimeMS(0) { }

    Status LiteParsedQuery::init(const string& ns, int ntoskip, int ntoreturn, int queryOptions,
                                 const BSONObj& queryObj, const BSONObj& proj,
                                 bool fromQueryMessage) {
        _ns = ns;
        _ntoskip = ntoskip;
        _ntoreturn = ntoreturn;
        _options = queryOptions;
        _proj = proj.getOwned();

        if (_ntoskip < 0) {
            return Status(ErrorCodes::BadValue, "bad skip value in query");
        }

        if (_ntoreturn == std::numeric_limits<int>::min()) {
            // _ntoreturn is negative but can't be negated.
            return Status(ErrorCodes::BadValue, "bad limit value in query");
        }

        if (_ntoreturn < 0) {
            // _ntoreturn greater than zero is simply a hint on how many objects to send back per
            // "cursor batch".  A negative number indicates a hard limit.
            _wantMore = false;
            _ntoreturn = -_ntoreturn;
        }

        if (fromQueryMessage) {
            BSONElement queryField = queryObj["query"];
            if (!queryField.isABSONObj()) { queryField = queryObj["$query"]; }
            if (queryField.isABSONObj()) {
                _filter = queryField.embeddedObject().getOwned();
                Status status = initFullQuery(queryObj);
                if (!status.isOK()) { return status; }
            }
            else {
                // TODO: Does this ever happen?
                _filter = queryObj.getOwned();
            }
        }
        else {
            // This is the debugging code path.
            _filter = queryObj.getOwned();
        }

        _hasReadPref = queryObj.hasField("$readPreference");

        if (!_sort.isEmpty()) {
            if (!isValidSortOrder(_sort)) {
                return Status(ErrorCodes::BadValue, "bad sort specification");
            }
            _sort = normalizeSortOrder(_sort);
        }

        // Min and Max objects must have the same fields.
        if (!_min.isEmpty() && !_max.isEmpty()) {
            if (!_min.isFieldNamePrefixOf(_max) || (_min.nFields() != _max.nFields())) {
                return Status(ErrorCodes::BadValue, "min and max must have the same field names");
            }
        }

        // Can't combine a normal sort and a $meta projection on the same field.
        BSONObjIterator projIt(_proj);
        while (projIt.more()) {
            BSONElement projElt = projIt.next();
            if (isTextScoreMeta(projElt)) {
                BSONElement sortElt = _sort[projElt.fieldName()];
                if (!sortElt.eoo() && !isTextScoreMeta(sortElt)) {
                    return Status(ErrorCodes::BadValue,
                                  "can't have a non-$meta sort on a $meta projection");
                }
            }
        }

        // All fields with a $meta sort must have a corresponding $meta projection.
        BSONObjIterator sortIt(_sort);
        while (sortIt.more()) {
            BSONElement sortElt = sortIt.next();
            if (isTextScoreMeta(sortElt)) {
                BSONElement projElt = _proj[sortElt.fieldName()];
                if (projElt.eoo() || !isTextScoreMeta(projElt)) {
                    return Status(ErrorCodes::BadValue,
                                  "must have $meta projection for all $meta sort keys");
                }
            }
        }

        return Status::OK();
    }

    Status LiteParsedQuery::initFullQuery(const BSONObj& top) {
        BSONObjIterator i(top);

        while (i.more()) {
            BSONElement e = i.next();
            const char* name = e.fieldName();
            
            if (0 == strcmp("$orderby", name) || 0 == strcmp("orderby", name)) {
                if (Object == e.type()) {
                    _sort = e.embeddedObject();
                }
                else if (Array == e.type()) {
                    _sort = e.embeddedObject();

                    // TODO: Is this ever used?  I don't think so.
                    // Quote:
                    // This is for languages whose "objects" are not well ordered (JSON is well
                    // ordered).
                    // [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
                    // note: this is slow, but that is ok as order will have very few pieces
                    BSONObjBuilder b;
                    char p[2] = "0";

                    while (1) {
                        BSONObj j = _sort.getObjectField(p);
                        if (j.isEmpty()) { break; }
                        BSONElement e = j.firstElement();
                        if (e.eoo()) {
                            return Status(ErrorCodes::BadValue, "bad order array");
                        }
                        if (!e.isNumber()) {
                            return Status(ErrorCodes::BadValue, "bad order array [2]");
                        }
                        b.append(e);
                        (*p)++;
                        if (!(*p <= '9')) {
                            return Status(ErrorCodes::BadValue, "too many ordering elements");
                        }
                    }

                    _sort = b.obj();
                }
                else {
                    return Status(ErrorCodes::BadValue, "sort must be object or array");
                }
            }
            else if ('$' == *name) {
                name++;
                if (str::equals("explain", name)) {
                    // Won't throw.
                    _explain = e.trueValue();
                }
                else if (str::equals("snapshot", name)) {
                    // Won't throw.
                    _snapshot = e.trueValue();
                }
                else if (str::equals("min", name)) {
                    if (!e.isABSONObj()) {
                        return Status(ErrorCodes::BadValue, "$min must be a BSONObj");
                    }
                    _min = e.embeddedObject();
                }
                else if (str::equals("max", name)) {
                    if (!e.isABSONObj()) {
                        return Status(ErrorCodes::BadValue, "$max must be a BSONObj");
                    }
                    _max = e.embeddedObject();
                }
                else if (str::equals("hint", name)) {
                    if (e.isABSONObj()) {
                        _hint = e.embeddedObject();
                    }
                    else {
                        // Hint can be specified as an object or as a string.  Wrap takes care of
                        // it.
                        _hint = e.wrap();
                    }
                }
                else if (str::equals("returnKey", name)) {
                    // Won't throw.
                    if (e.trueValue()) {
                        _returnKey = true;
                        BSONObjBuilder projBob;
                        projBob.appendElements(_proj);
                        // We use $$ because it's never going to show up in a user's projection.
                        // The exact text doesn't matter.
                        BSONObj indexKey = BSON("$$" <<
                                                BSON("$meta" << LiteParsedQuery::metaIndexKey));
                        projBob.append(indexKey.firstElement());
                        _proj = projBob.obj();
                    }
                }
                else if (str::equals("maxScan", name)) {
                    // Won't throw.
                    _maxScan = e.numberInt();
                }
                else if (str::equals("showDiskLoc", name)) {
                    // Won't throw.
                    if (e.trueValue()) {
                        BSONObjBuilder projBob;
                        projBob.appendElements(_proj);
                        BSONObj metaDiskLoc = BSON("$diskLoc" <<
                                                   BSON("$meta" << LiteParsedQuery::metaDiskLoc));
                        projBob.append(metaDiskLoc.firstElement());
                        _proj = projBob.obj();
                    }
                }
                else if (str::equals("maxTimeMS", name)) {
                    StatusWith<int> maxTimeMS = parseMaxTimeMS(e);
                    if (!maxTimeMS.isOK()) {
                        return maxTimeMS.getStatus();
                    }
                    _maxTimeMS = maxTimeMS.getValue();
                }
            }
        }
        
        if (_snapshot) {
            if (!_sort.isEmpty()) {
                return Status(ErrorCodes::BadValue, "E12001 can't use sort with $snapshot");
            }
            if (!_hint.isEmpty()) {
                return Status(ErrorCodes::BadValue, "E12002 can't use hint with $snapshot");
            }
        }

        return Status::OK();
    }

} // namespace mongo
