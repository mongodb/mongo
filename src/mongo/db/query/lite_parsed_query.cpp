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

#include "mongo/db/dbmessage.h"
#include "mongo/util/assert_util.h"

namespace mongo {

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
                                 LiteParsedQuery** out) {
        auto_ptr<LiteParsedQuery> pq(new LiteParsedQuery());
        pq->_sort = sort;

        Status status = pq->init(ns, ntoskip, ntoreturn, queryOptions, query, proj, false);
        if (status.isOK()) { *out = pq.release(); }
        return status;
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
        return StatusWith<int>(static_cast<int>(maxTimeMSLongLong));
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
        _proj = proj;

        if (_ntoskip < 0) {
            return Status(ErrorCodes::BadValue, "bad skip value in query");
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
                    if (!e.isABSONObj()) {
                        return Status(ErrorCodes::BadValue, "$hint must be a BSONObj");
                    }
                    _hint = e.embeddedObject();
                }
                else if (str::equals("returnKey", name)) {
                    // Won't throw.
                    _returnKey = e.trueValue();
                }
                else if (str::equals("maxScan", name)) {
                    // Won't throw.
                    _maxScan = e.numberInt();
                }
                else if (str::equals("showDiskLoc", name)) {
                    // Won't throw.
                    _showDiskLoc = e.trueValue();
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
