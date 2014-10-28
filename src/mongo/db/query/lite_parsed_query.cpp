/**
 *    Copyright 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/query/lite_parsed_query.h"

#include <cmath>

#include "mongo/db/dbmessage.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const string LiteParsedQuery::cmdOptionMaxTimeMS("maxTimeMS");
    const string LiteParsedQuery::queryOptionMaxTimeMS("$maxTimeMS");

    const string LiteParsedQuery::metaTextScore("textScore");
    const string LiteParsedQuery::metaGeoNearDistance("geoNearDistance");
    const string LiteParsedQuery::metaGeoNearPoint("geoNearPoint");
    const string LiteParsedQuery::metaDiskLoc("diskloc");
    const string LiteParsedQuery::metaIndexKey("indexKey");

    namespace {

        Status checkFieldType(const BSONElement& el, BSONType type) {
            if (type != el.type()) {
                mongoutils::str::stream ss;
                ss << "Failed to parse: " << el.toString() << ". "
                   << "'" << el.fieldName() << "' field must be of BSON type "
                   << typeName(type) << ".";
                return Status(ErrorCodes::BadValue, ss);
            }

            return Status::OK();
        }

    } // namespace

    // static
    Status LiteParsedQuery::make(const std::string& fullns,
                                 const BSONObj& cmdObj,
                                 bool isExplain,
                                 LiteParsedQuery** out) {
        auto_ptr<LiteParsedQuery> pq(new LiteParsedQuery());
        pq->_ns = fullns;
        pq->_explain = isExplain;

        // Parse the command BSON by looping through one element at a time.
        BSONObjIterator it(cmdObj);
        while (it.more()) {
            BSONElement el = it.next();
            const char* fieldName = el.fieldName();
            if (mongoutils::str::equals(fieldName, "find")) {
                // We've already parsed the namespace information contained in the 'find'
                // field, so just move on.
                continue;
            }
            else if (mongoutils::str::equals(fieldName, "filter")) {
                Status status = checkFieldType(el, Object);
                if (!status.isOK()) {
                    return status;
                }

                pq->_filter = el.Obj().getOwned();
            }
            else if (mongoutils::str::equals(fieldName, "sort")) {
                Status status = checkFieldType(el, Object);
                if (!status.isOK()) {
                    return status;
                }

                // Sort document normalization.
                BSONObj sort = el.Obj().getOwned();
                if (!isValidSortOrder(sort)) {
                    return Status(ErrorCodes::BadValue, "bad sort specification");
                }

                pq->_sort = sort;
            }
            else if (mongoutils::str::equals(fieldName, "projection")) {
                Status status = checkFieldType(el, Object);
                if (!status.isOK()) {
                    return status;
                }

                pq->_proj = el.Obj().getOwned();
            }
            else if (mongoutils::str::equals(fieldName, "hint")) {
                BSONObj hintObj;
                if (Object == el.type()) {
                    hintObj = cmdObj["hint"].Obj().getOwned();
                }
                else if (String == el.type()) {
                    hintObj = el.wrap();
                }
                else {
                    return Status(ErrorCodes::BadValue,
                                  "hint must be either a string or nested object");
                }

                pq->_hint = hintObj;
            }
            else if (mongoutils::str::equals(fieldName, "skip")) {
                if (!el.isNumber()) {
                    mongoutils::str::stream ss;
                    ss << "Failed to parse: " << cmdObj.toString() << ". "
                       << "'skip' field must be numeric.";
                    return Status(ErrorCodes::BadValue, ss);
                }

                int skip = el.numberInt();
                if (skip < 0) {
                    return Status(ErrorCodes::BadValue, "skip value must be non-negative");
                }

                pq->_skip = skip;
            }
            else if (mongoutils::str::equals(fieldName, "limit")) {
                if (!el.isNumber()) {
                    mongoutils::str::stream ss;
                    ss << "Failed to parse: " << cmdObj.toString() << ". "
                       << "'limit' field must be numeric.";
                    return Status(ErrorCodes::BadValue, ss);
                }

                int limit = el.numberInt();
                if (limit < 0) {
                    return Status(ErrorCodes::BadValue, "limit value must be non-negative");
                }

                pq->_limit = limit;
            }
            else if (mongoutils::str::equals(fieldName, "batchSize")) {
                if (!el.isNumber()) {
                    mongoutils::str::stream ss;
                    ss << "Failed to parse: " << cmdObj.toString() << ". "
                       << "'batchSize' field must be numeric.";
                    return Status(ErrorCodes::BadValue, ss);
                }

                int batchSize = el.numberInt();
                if (batchSize <= 0) {
                    return Status(ErrorCodes::BadValue, "batchSize value must be positive");
                }

                pq->_batchSize = batchSize;
            }
            else if (mongoutils::str::equals(fieldName, "singleBatch")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                pq->_wantMore = !el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "options")) {
                Status status = checkFieldType(el, Object);
                if (!status.isOK()) {
                    return status;
                }

                Status parseStatus = Options::parseFromBSON(el.Obj(), &pq->_options);
                if (!parseStatus.isOK()) {
                    return parseStatus;
                }
            }
            else if (mongoutils::str::equals(fieldName, "$readPreference")) {
                pq->_options.hasReadPref = true;
            }
            else {
                mongoutils::str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "Unrecognized field '" << fieldName << "'.";
                return Status(ErrorCodes::BadValue, ss);
            }
        }

        // We might need to update the projection object with a $meta projection.
        if (pq->getOptions().returnKey) {
            pq->addReturnKeyMetaProj();
        }
        if (pq->getOptions().showDiskLoc) {
            pq->addShowDiskLocMetaProj();
        }

        Status validateStatus = pq->validate();
        if (!validateStatus.isOK()) {
            return validateStatus;
        }

        *out = pq.release();
        return Status::OK();
    }

    void LiteParsedQuery::addReturnKeyMetaProj() {
        BSONObjBuilder projBob;
        projBob.appendElements(_proj);
        // We use $$ because it's never going to show up in a user's projection.
        // The exact text doesn't matter.
        BSONObj indexKey = BSON("$$" <<
                                BSON("$meta" << LiteParsedQuery::metaIndexKey));
        projBob.append(indexKey.firstElement());
        _proj = projBob.obj();
    }

    void LiteParsedQuery::addShowDiskLocMetaProj() {
        BSONObjBuilder projBob;
        projBob.appendElements(_proj);
        BSONObj metaDiskLoc = BSON("$diskLoc" <<
                                   BSON("$meta" << LiteParsedQuery::metaDiskLoc));
        projBob.append(metaDiskLoc.firstElement());
        _proj = projBob.obj();
    }

    Status LiteParsedQuery::validate() const {
        // Min and Max objects must have the same fields.
        if (!_options.min.isEmpty() && !_options.max.isEmpty()) {
            if (!_options.min.isFieldNamePrefixOf(_options.max) ||
                (_options.min.nFields() != _options.max.nFields())) {
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

        if (_options.snapshot) {
            if (!_sort.isEmpty()) {
                return Status(ErrorCodes::BadValue, "E12001 can't use sort with $snapshot");
            }
            if (!_hint.isEmpty()) {
                return Status(ErrorCodes::BadValue, "E12002 can't use hint with $snapshot");
            }
        }

        return Status::OK();
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

    LiteParsedQuery::LiteParsedQuery() :
        _skip(0),
        _limit(0),
        _batchSize(101),
        _wantMore(true),
        _explain(false) { }

    //
    // LiteParsedQuery::Options
    //

    LiteParsedQuery::Options::Options() {
        clear();
    }

    void LiteParsedQuery::Options::clear() {
        this->maxScan = 0;
        this->maxTimeMS = 0,
        this->returnKey = false;
        this->showDiskLoc = false;
        this->snapshot = false;
        this->hasReadPref = false;
        this->tailable = false;
        this->slaveOk = false;
        this->oplogReplay = false;
        this->noCursorTimeout = false;
        this->awaitData = false;
        this->exhaust = false;
        this->partial = false;
    }

    void LiteParsedQuery::Options::initFromInt(int options) {
        this->tailable = (options & QueryOption_CursorTailable) != 0;
        this->slaveOk = (options & QueryOption_SlaveOk) != 0;
        this->oplogReplay = (options & QueryOption_OplogReplay) != 0;
        this->noCursorTimeout = (options & QueryOption_NoCursorTimeout) != 0;
        this->awaitData = (options & QueryOption_AwaitData) != 0;
        this->exhaust = (options & QueryOption_Exhaust) != 0;
        this->partial = (options & QueryOption_PartialResults) != 0;
    }

    int LiteParsedQuery::Options::toInt() const {
        int options = 0;
        if (this->tailable) { options |= QueryOption_CursorTailable; }
        if (this->slaveOk) { options |= QueryOption_SlaveOk; }
        if (this->oplogReplay) { options |= QueryOption_OplogReplay; }
        if (this->noCursorTimeout) { options |= QueryOption_NoCursorTimeout; }
        if (this->awaitData) { options |= QueryOption_AwaitData; }
        if (this->exhaust) { options |= QueryOption_Exhaust; }
        if (this->partial) { options |= QueryOption_PartialResults; }
        return options;
    }

    // static
    Status LiteParsedQuery::Options::parseFromBSON(const BSONObj& optionsObj,
                                                   LiteParsedQuery::Options* out) {
        BSONObjIterator it(optionsObj);
        while (it.more()) {
            BSONElement el = it.next();
            const char* fieldName = el.fieldName();
            if (mongoutils::str::equals(fieldName, "comment")) {
                Status status = checkFieldType(el, String);
                if (!status.isOK()) {
                    return status;
                }

                out->comment = el.str();
            }
            else if (mongoutils::str::equals(fieldName, "maxScan")) {
                if (!el.isNumber()) {
                    mongoutils::str::stream ss;
                    ss << "Failed to parse: " << optionsObj.toString() << ". "
                       << "'maxScan' field must be numeric.";
                    return Status(ErrorCodes::BadValue, ss);
                }

                int maxScan = el.numberInt();
                if (maxScan < 0) {
                    return Status(ErrorCodes::BadValue, "maxScan value must be non-negative");
                }

                out->maxScan = maxScan;
            }
            else if (mongoutils::str::equals(fieldName, cmdOptionMaxTimeMS.c_str())) {
                StatusWith<int> maxTimeMS = parseMaxTimeMS(el);
                if (!maxTimeMS.isOK()) {
                    return maxTimeMS.getStatus();
                }

                out->maxTimeMS = maxTimeMS.getValue();
            }
            else if (mongoutils::str::equals(fieldName, "min")) {
                Status status = checkFieldType(el, Object);
                if (!status.isOK()) {
                    return status;
                }

                out->min = el.Obj().getOwned();
            }
            else if (mongoutils::str::equals(fieldName, "max")) {
                Status status = checkFieldType(el, Object);
                if (!status.isOK()) {
                    return status;
                }

                out->max = el.Obj().getOwned();
            }
            else if (mongoutils::str::equals(fieldName, "returnKey")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->returnKey = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "showDiskLoc")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->showDiskLoc = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "snapshot")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->snapshot = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "tailable")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->tailable = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "slaveOk")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->slaveOk = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "oplogReplay")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->oplogReplay = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "noCursorTimeout")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->noCursorTimeout = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "awaitData")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->awaitData = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "exhaust")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->exhaust = el.boolean();
            }
            else if (mongoutils::str::equals(fieldName, "partial")) {
                Status status = checkFieldType(el, Bool);
                if (!status.isOK()) {
                    return status;
                }

                out->partial = el.boolean();
            }
            else {
                mongoutils::str::stream ss;
                ss << "Failed to parse query options: " << optionsObj.toString() << ". "
                   << "Unrecognized option field '" << fieldName << "'.";
                return Status(ErrorCodes::BadValue, ss);
            }
        }

        return Status::OK();
    }

    //
    // Old LiteParsedQuery parsing code: SOON TO BE DEPRECATED.
    //

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
        pq->_sort = sort.getOwned();
        pq->_hint = hint.getOwned();
        pq->_options.min = minObj.getOwned();
        pq->_options.max = maxObj.getOwned();
        pq->_options.snapshot = snapshot;
        pq->_explain = explain;

        Status status = pq->init(ns, ntoskip, ntoreturn, queryOptions, query, proj, false);
        if (status.isOK()) { *out = pq.release(); }
        return status;
    }

    Status LiteParsedQuery::init(const string& ns, int ntoskip, int ntoreturn, int queryOptions,
                                 const BSONObj& queryObj, const BSONObj& proj,
                                 bool fromQueryMessage) {
        _ns = ns;
        _skip = ntoskip;
        _limit = ntoreturn;
        _options.initFromInt(queryOptions);
        _proj = proj.getOwned();

        if (_skip < 0) {
            return Status(ErrorCodes::BadValue, "bad skip value in query");
        }

        if (_limit == std::numeric_limits<int>::min()) {
            // _limit is negative but can't be negated.
            return Status(ErrorCodes::BadValue, "bad limit value in query");
        }

        if (_limit < 0) {
            // _limit greater than zero is simply a hint on how many objects to send back per
            // "cursor batch".  A negative number indicates a hard limit.
            _wantMore = false;
            _limit = -_limit;
        }

        // We are constructing this LiteParsedQuery from a legacy OP_QUERY message, and therefore
        // cannot distinguish batchSize and limit. (They are a single field in OP_QUERY, but are
        // passed separately for the find command.) Just set both values to be the same.
        _batchSize = _limit;

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

        _options.hasReadPref = queryObj.hasField("$readPreference");

        if (!isValidSortOrder(_sort)) {
            return Status(ErrorCodes::BadValue, "bad sort specification");
        }

        return validate();
    }

    Status LiteParsedQuery::initFullQuery(const BSONObj& top) {
        BSONObjIterator i(top);

        while (i.more()) {
            BSONElement e = i.next();
            const char* name = e.fieldName();

            if (0 == strcmp("$orderby", name) || 0 == strcmp("orderby", name)) {
                if (Object == e.type()) {
                    _sort = e.embeddedObject().getOwned();
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
                    _options.snapshot = e.trueValue();
                }
                else if (str::equals("min", name)) {
                    if (!e.isABSONObj()) {
                        return Status(ErrorCodes::BadValue, "$min must be a BSONObj");
                    }
                    _options.min = e.embeddedObject().getOwned();
                }
                else if (str::equals("max", name)) {
                    if (!e.isABSONObj()) {
                        return Status(ErrorCodes::BadValue, "$max must be a BSONObj");
                    }
                    _options.max = e.embeddedObject().getOwned();
                }
                else if (str::equals("hint", name)) {
                    if (e.isABSONObj()) {
                        _hint = e.embeddedObject().getOwned();
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
                        _options.returnKey = true;
                        addReturnKeyMetaProj();
                    }
                }
                else if (str::equals("maxScan", name)) {
                    // Won't throw.
                    _options.maxScan = e.numberInt();
                }
                else if (str::equals("showDiskLoc", name)) {
                    // Won't throw.
                    if (e.trueValue()) {
                        _options.showDiskLoc = true;
                        addShowDiskLocMetaProj();
                    }
                }
                else if (str::equals("maxTimeMS", name)) {
                    StatusWith<int> maxTimeMS = parseMaxTimeMS(e);
                    if (!maxTimeMS.isOK()) {
                        return maxTimeMS.getStatus();
                    }
                    _options.maxTimeMS = maxTimeMS.getValue();
                }
            }
        }

        return Status::OK();
    }

} // namespace mongo
