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

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_request.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::unique_ptr;

const std::string QueryRequest::kUnwrappedReadPrefField("$queryOptions");
const std::string QueryRequest::kWrappedReadPrefField("$readPreference");

const char QueryRequest::cmdOptionMaxTimeMS[] = "maxTimeMS";
const char QueryRequest::queryOptionMaxTimeMS[] = "$maxTimeMS";

const string QueryRequest::metaGeoNearDistance("geoNearDistance");
const string QueryRequest::metaGeoNearPoint("geoNearPoint");
const string QueryRequest::metaIndexKey("indexKey");
const string QueryRequest::metaRecordId("recordId");
const string QueryRequest::metaSortKey("sortKey");
const string QueryRequest::metaTextScore("textScore");

const long long QueryRequest::kDefaultBatchSize = 101;

namespace {

Status checkFieldType(const BSONElement& el, BSONType type) {
    if (type != el.type()) {
        str::stream ss;
        ss << "Failed to parse: " << el.toString() << ". "
           << "'" << el.fieldName() << "' field must be of BSON type " << typeName(type) << ".";
        return Status(ErrorCodes::FailedToParse, ss);
    }

    return Status::OK();
}

// Find command field names.
const char kFilterField[] = "filter";
const char kProjectionField[] = "projection";
const char kSortField[] = "sort";
const char kHintField[] = "hint";
const char kCollationField[] = "collation";
const char kSkipField[] = "skip";
const char kLimitField[] = "limit";
const char kBatchSizeField[] = "batchSize";
const char kNToReturnField[] = "ntoreturn";
const char kSingleBatchField[] = "singleBatch";
const char kCommentField[] = "comment";
const char kMaxScanField[] = "maxScan";
const char kMaxField[] = "max";
const char kMinField[] = "min";
const char kReturnKeyField[] = "returnKey";
const char kShowRecordIdField[] = "showRecordId";
const char kSnapshotField[] = "snapshot";
const char kTailableField[] = "tailable";
const char kOplogReplayField[] = "oplogReplay";
const char kNoCursorTimeoutField[] = "noCursorTimeout";
const char kAwaitDataField[] = "awaitData";
const char kPartialResultsField[] = "allowPartialResults";
const char kTermField[] = "term";
const char kOptionsField[] = "options";

}  // namespace

const char QueryRequest::kFindCommandName[] = "find";
const char QueryRequest::kShardVersionField[] = "shardVersion";

QueryRequest::QueryRequest(NamespaceString nss) : _nss(std::move(nss)) {}

// static
StatusWith<unique_ptr<QueryRequest>> QueryRequest::makeFromFindCommand(NamespaceString nss,
                                                                       const BSONObj& cmdObj,
                                                                       bool isExplain) {
    unique_ptr<QueryRequest> qr(new QueryRequest(std::move(nss)));
    qr->_explain = isExplain;

    // Parse the command BSON by looping through one element at a time.
    BSONObjIterator it(cmdObj);
    while (it.more()) {
        BSONElement el = it.next();
        const char* fieldName = el.fieldName();
        if (str::equals(fieldName, kFindCommandName)) {
            Status status = checkFieldType(el, String);
            if (!status.isOK()) {
                return status;
            }
        } else if (str::equals(fieldName, kFilterField)) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_filter = el.Obj().getOwned();
        } else if (str::equals(fieldName, kProjectionField)) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_proj = el.Obj().getOwned();
        } else if (str::equals(fieldName, kSortField)) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_sort = el.Obj().getOwned();
        } else if (str::equals(fieldName, kHintField)) {
            BSONObj hintObj;
            if (Object == el.type()) {
                hintObj = cmdObj["hint"].Obj().getOwned();
            } else if (String == el.type()) {
                hintObj = el.wrap("$hint");
            } else {
                return Status(ErrorCodes::FailedToParse,
                              "hint must be either a string or nested object");
            }

            qr->_hint = hintObj;
        } else if (str::equals(fieldName, repl::ReadConcernArgs::kReadConcernFieldName.c_str())) {
            // Read concern parsing is handled elsewhere, but we store a copy here.
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_readConcern = el.Obj().getOwned();
        } else if (str::equals(fieldName, kCollationField)) {
            // Collation parsing is handled elsewhere, but we store a copy here.
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_collation = el.Obj().getOwned();
        } else if (str::equals(fieldName, kSkipField)) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'skip' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            long long skip = el.numberLong();

            // A skip value of 0 means that there is no skip.
            if (skip) {
                qr->_skip = skip;
            }
        } else if (str::equals(fieldName, kLimitField)) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'limit' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            long long limit = el.numberLong();

            // A limit value of 0 means that there is no limit.
            if (limit) {
                qr->_limit = limit;
            }
        } else if (str::equals(fieldName, kBatchSizeField)) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'batchSize' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            qr->_batchSize = el.numberLong();
        } else if (str::equals(fieldName, kNToReturnField)) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'ntoreturn' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            qr->_ntoreturn = el.numberLong();
        } else if (str::equals(fieldName, kSingleBatchField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_wantMore = !el.boolean();
        } else if (str::equals(fieldName, kCommentField)) {
            Status status = checkFieldType(el, String);
            if (!status.isOK()) {
                return status;
            }

            qr->_comment = el.str();
        } else if (str::equals(fieldName, kMaxScanField)) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'maxScan' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            qr->_maxScan = el.numberInt();
        } else if (str::equals(fieldName, cmdOptionMaxTimeMS)) {
            StatusWith<int> maxTimeMS = parseMaxTimeMS(el);
            if (!maxTimeMS.isOK()) {
                return maxTimeMS.getStatus();
            }

            qr->_maxTimeMS = maxTimeMS.getValue();
        } else if (str::equals(fieldName, kMinField)) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_min = el.Obj().getOwned();
        } else if (str::equals(fieldName, kMaxField)) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_max = el.Obj().getOwned();
        } else if (str::equals(fieldName, kReturnKeyField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_returnKey = el.boolean();
        } else if (str::equals(fieldName, kShowRecordIdField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_showRecordId = el.boolean();
        } else if (str::equals(fieldName, kSnapshotField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_snapshot = el.boolean();
        } else if (str::equals(fieldName, kTailableField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_tailable = el.boolean();
        } else if (str::equals(fieldName, kOplogReplayField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_oplogReplay = el.boolean();
        } else if (str::equals(fieldName, kNoCursorTimeoutField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_noCursorTimeout = el.boolean();
        } else if (str::equals(fieldName, kAwaitDataField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_awaitData = el.boolean();
        } else if (str::equals(fieldName, kPartialResultsField)) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_allowPartialResults = el.boolean();
        } else if (str::equals(fieldName, kOptionsField)) {
            // 3.0.x versions of the shell may generate an explain of a find command with an
            // 'options' field. We accept this only if the 'options' field is empty so that
            // the shell's explain implementation is forwards compatible.
            //
            // TODO: Remove for 3.4.
            if (!qr->isExplain()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Field '" << kOptionsField
                                            << "' is only allowed for explain.");
            }

            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            BSONObj optionsObj = el.Obj();
            if (!optionsObj.isEmpty()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Failed to parse options: " << optionsObj.toString()
                                            << ". You may need to update your shell or driver.");
            }
        } else if (str::equals(fieldName, kShardVersionField)) {
            // Shard version parsing is handled elsewhere.
        } else if (str::equals(fieldName, kTermField)) {
            Status status = checkFieldType(el, NumberLong);
            if (!status.isOK()) {
                return status;
            }
            qr->_replicationTerm = el._numberLong();
        } else if (!str::startsWith(fieldName, '$')) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Failed to parse: " << cmdObj.toString() << ". "
                                        << "Unrecognized field '"
                                        << fieldName
                                        << "'.");
        }
    }

    qr->addMetaProjection();

    Status validateStatus = qr->validate();
    if (!validateStatus.isOK()) {
        return validateStatus;
    }

    return std::move(qr);
}

BSONObj QueryRequest::asFindCommand() const {
    BSONObjBuilder bob;
    asFindCommand(&bob);
    return bob.obj();
}

void QueryRequest::asFindCommand(BSONObjBuilder* cmdBuilder) const {
    cmdBuilder->append(kFindCommandName, _nss.coll());

    if (!_filter.isEmpty()) {
        cmdBuilder->append(kFilterField, _filter);
    }

    if (!_proj.isEmpty()) {
        cmdBuilder->append(kProjectionField, _proj);
    }

    if (!_sort.isEmpty()) {
        cmdBuilder->append(kSortField, _sort);
    }

    if (!_hint.isEmpty()) {
        cmdBuilder->append(kHintField, _hint);
    }

    if (!_readConcern.isEmpty()) {
        cmdBuilder->append(repl::ReadConcernArgs::kReadConcernFieldName, _readConcern);
    }

    if (!_collation.isEmpty()) {
        cmdBuilder->append(kCollationField, _collation);
    }

    if (_skip) {
        cmdBuilder->append(kSkipField, *_skip);
    }

    if (_ntoreturn) {
        cmdBuilder->append(kNToReturnField, *_ntoreturn);
    }

    if (_limit) {
        cmdBuilder->append(kLimitField, *_limit);
    }

    if (_batchSize) {
        cmdBuilder->append(kBatchSizeField, *_batchSize);
    }

    if (!_wantMore) {
        cmdBuilder->append(kSingleBatchField, true);
    }

    if (!_comment.empty()) {
        cmdBuilder->append(kCommentField, _comment);
    }

    if (_maxScan > 0) {
        cmdBuilder->append(kMaxScanField, _maxScan);
    }

    if (_maxTimeMS > 0) {
        cmdBuilder->append(cmdOptionMaxTimeMS, _maxTimeMS);
    }

    if (!_max.isEmpty()) {
        cmdBuilder->append(kMaxField, _max);
    }

    if (!_min.isEmpty()) {
        cmdBuilder->append(kMinField, _min);
    }

    if (_returnKey) {
        cmdBuilder->append(kReturnKeyField, true);
    }

    if (_showRecordId) {
        cmdBuilder->append(kShowRecordIdField, true);
    }

    if (_snapshot) {
        cmdBuilder->append(kSnapshotField, true);
    }

    if (_tailable) {
        cmdBuilder->append(kTailableField, true);
    }

    if (_oplogReplay) {
        cmdBuilder->append(kOplogReplayField, true);
    }

    if (_noCursorTimeout) {
        cmdBuilder->append(kNoCursorTimeoutField, true);
    }

    if (_awaitData) {
        cmdBuilder->append(kAwaitDataField, true);
    }

    if (_allowPartialResults) {
        cmdBuilder->append(kPartialResultsField, true);
    }

    if (_replicationTerm) {
        cmdBuilder->append(kTermField, *_replicationTerm);
    }
}

void QueryRequest::addReturnKeyMetaProj() {
    BSONObjBuilder projBob;
    projBob.appendElements(_proj);
    // We use $$ because it's never going to show up in a user's projection.
    // The exact text doesn't matter.
    BSONObj indexKey = BSON("$$" << BSON("$meta" << QueryRequest::metaIndexKey));
    projBob.append(indexKey.firstElement());
    _proj = projBob.obj();
}

void QueryRequest::addShowRecordIdMetaProj() {
    BSONObjBuilder projBob;
    projBob.appendElements(_proj);
    BSONObj metaRecordId = BSON("$recordId" << BSON("$meta" << QueryRequest::metaRecordId));
    projBob.append(metaRecordId.firstElement());
    _proj = projBob.obj();
}

Status QueryRequest::validate() const {
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

    if (!isValidSortOrder(_sort)) {
        return Status(ErrorCodes::BadValue, "bad sort specification");
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

    if (_snapshot) {
        if (!_sort.isEmpty()) {
            return Status(ErrorCodes::BadValue, "E12001 can't use sort with snapshot");
        }
        if (!_hint.isEmpty()) {
            return Status(ErrorCodes::BadValue, "E12002 can't use hint with snapshot");
        }
    }

    if ((_limit || _batchSize) && _ntoreturn) {
        return Status(ErrorCodes::BadValue,
                      "'limit' or 'batchSize' fields can not be set with 'ntoreturn' field.");
    }


    if (_skip && *_skip < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Skip value must be non-negative, but received: " << *_skip);
    }

    if (_limit && *_limit < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Limit value must be non-negative, but received: "
                                    << *_limit);
    }

    if (_batchSize && *_batchSize < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "BatchSize value must be non-negative, but received: "
                                    << *_batchSize);
    }

    if (_ntoreturn && *_ntoreturn < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "NToReturn value must be non-negative, but received: "
                                    << *_ntoreturn);
    }

    if (_maxScan < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "MaxScan value must be non-negative, but received: "
                                    << _maxScan);
    }

    if (_maxTimeMS < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "MaxTimeMS value must be non-negative, but received: "
                                    << _maxTimeMS);
    }

    if (_tailable) {
        // Tailable cursors cannot have any sort other than {$natural: 1}.
        const BSONObj expectedSort = BSON("$natural" << 1);
        if (!_sort.isEmpty() && _sort != expectedSort) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with a sort other than {$natural: 1}");
        }

        // Cannot indicate that you want a 'singleBatch' if the cursor is tailable.
        if (!_wantMore) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with the 'singleBatch' option");
        }
    }

    if (_awaitData && !_tailable) {
        return Status(ErrorCodes::BadValue, "Cannot set awaitData without tailable");
    }

    return Status::OK();
}

// static
StatusWith<int> QueryRequest::parseMaxTimeMS(BSONElement maxTimeMSElt) {
    if (!maxTimeMSElt.eoo() && !maxTimeMSElt.isNumber()) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " must be a number").str());
    }
    long long maxTimeMSLongLong = maxTimeMSElt.safeNumberLong();  // returns 0 on EOO
    if (maxTimeMSLongLong < 0 || maxTimeMSLongLong > INT_MAX) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " is out of range").str());
    }
    double maxTimeMSDouble = maxTimeMSElt.numberDouble();
    if (maxTimeMSElt.type() == mongo::NumberDouble && floor(maxTimeMSDouble) != maxTimeMSDouble) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " has non-integral value")
                .str());
    }
    return StatusWith<int>(static_cast<int>(maxTimeMSLongLong));
}

// static
bool QueryRequest::isTextScoreMeta(BSONElement elt) {
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
    if (!str::equals("$meta", metaElt.fieldName())) {
        return false;
    }
    if (mongo::String != metaElt.type()) {
        return false;
    }
    if (QueryRequest::metaTextScore != metaElt.valuestr()) {
        return false;
    }
    // must have exactly 1 element
    if (metaIt.more()) {
        return false;
    }
    return true;
}

// static
bool QueryRequest::isValidSortOrder(const BSONObj& sortObj) {
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
bool QueryRequest::isQueryIsolated(const BSONObj& query) {
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

//
// Old QueryRequest parsing code: SOON TO BE DEPRECATED.
//

// static
StatusWith<unique_ptr<QueryRequest>> QueryRequest::fromLegacyQueryMessage(const QueryMessage& qm) {
    unique_ptr<QueryRequest> qr(new QueryRequest(NamespaceString(qm.ns)));

    Status status = qr->init(qm.ntoskip, qm.ntoreturn, qm.queryOptions, qm.query, qm.fields, true);
    if (!status.isOK()) {
        return status;
    }

    return std::move(qr);
}

Status QueryRequest::init(int ntoskip,
                          int ntoreturn,
                          int queryOptions,
                          const BSONObj& queryObj,
                          const BSONObj& proj,
                          bool fromQueryMessage) {
    _proj = proj.getOwned();

    if (ntoskip) {
        _skip = ntoskip;
    }

    if (ntoreturn) {
        if (ntoreturn < 0) {
            if (ntoreturn == std::numeric_limits<int>::min()) {
                // ntoreturn is negative but can't be negated.
                return Status(ErrorCodes::BadValue, "bad ntoreturn value in query");
            }
            _ntoreturn = -ntoreturn;
            _wantMore = false;
        } else {
            _ntoreturn = ntoreturn;
        }
    }

    // Initialize flags passed as 'queryOptions' bit vector.
    initFromInt(queryOptions);

    if (fromQueryMessage) {
        BSONElement queryField = queryObj["query"];
        if (!queryField.isABSONObj()) {
            queryField = queryObj["$query"];
        }
        if (queryField.isABSONObj()) {
            _filter = queryField.embeddedObject().getOwned();
            Status status = initFullQuery(queryObj);
            if (!status.isOK()) {
                return status;
            }
        } else {
            _filter = queryObj.getOwned();
        }
    } else {
        // This is the debugging code path.
        _filter = queryObj.getOwned();
    }

    _hasReadPref = queryObj.hasField("$readPreference");

    return validate();
}

Status QueryRequest::initFullQuery(const BSONObj& top) {
    BSONObjIterator i(top);

    while (i.more()) {
        BSONElement e = i.next();
        const char* name = e.fieldName();

        if (0 == strcmp("$orderby", name) || 0 == strcmp("orderby", name)) {
            if (Object == e.type()) {
                _sort = e.embeddedObject().getOwned();
            } else if (Array == e.type()) {
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
                    if (j.isEmpty()) {
                        break;
                    }
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
            } else {
                return Status(ErrorCodes::BadValue, "sort must be object or array");
            }
        } else if ('$' == *name) {
            name++;
            if (str::equals("explain", name)) {
                // Won't throw.
                _explain = e.trueValue();
            } else if (str::equals("snapshot", name)) {
                // Won't throw.
                _snapshot = e.trueValue();
            } else if (str::equals("min", name)) {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$min must be a BSONObj");
                }
                _min = e.embeddedObject().getOwned();
            } else if (str::equals("max", name)) {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$max must be a BSONObj");
                }
                _max = e.embeddedObject().getOwned();
            } else if (str::equals("hint", name)) {
                if (e.isABSONObj()) {
                    _hint = e.embeddedObject().getOwned();
                } else if (String == e.type()) {
                    _hint = e.wrap();
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "$hint must be either a string or nested object");
                }
            } else if (str::equals("returnKey", name)) {
                // Won't throw.
                if (e.trueValue()) {
                    _returnKey = true;
                    addReturnKeyMetaProj();
                }
            } else if (str::equals("maxScan", name)) {
                // Won't throw.
                _maxScan = e.numberInt();
            } else if (str::equals("showDiskLoc", name)) {
                // Won't throw.
                if (e.trueValue()) {
                    _showRecordId = true;
                    addShowRecordIdMetaProj();
                }
            } else if (str::equals("maxTimeMS", name)) {
                StatusWith<int> maxTimeMS = parseMaxTimeMS(e);
                if (!maxTimeMS.isOK()) {
                    return maxTimeMS.getStatus();
                }
                _maxTimeMS = maxTimeMS.getValue();
            }
        }
    }

    return Status::OK();
}

int QueryRequest::getOptions() const {
    int options = 0;
    if (_tailable) {
        options |= QueryOption_CursorTailable;
    }
    if (_slaveOk) {
        options |= QueryOption_SlaveOk;
    }
    if (_oplogReplay) {
        options |= QueryOption_OplogReplay;
    }
    if (_noCursorTimeout) {
        options |= QueryOption_NoCursorTimeout;
    }
    if (_awaitData) {
        options |= QueryOption_AwaitData;
    }
    if (_exhaust) {
        options |= QueryOption_Exhaust;
    }
    if (_allowPartialResults) {
        options |= QueryOption_PartialResults;
    }
    return options;
}

void QueryRequest::initFromInt(int options) {
    _tailable = (options & QueryOption_CursorTailable) != 0;
    _slaveOk = (options & QueryOption_SlaveOk) != 0;
    _oplogReplay = (options & QueryOption_OplogReplay) != 0;
    _noCursorTimeout = (options & QueryOption_NoCursorTimeout) != 0;
    _awaitData = (options & QueryOption_AwaitData) != 0;
    _exhaust = (options & QueryOption_Exhaust) != 0;
    _allowPartialResults = (options & QueryOption_PartialResults) != 0;
}

void QueryRequest::addMetaProjection() {
    // We might need to update the projection object with a $meta projection.
    if (returnKey()) {
        addReturnKeyMetaProj();
    }

    if (showRecordId()) {
        addShowRecordIdMetaProj();
    }
}

boost::optional<long long> QueryRequest::getEffectiveBatchSize() const {
    return _batchSize ? _batchSize : _ntoreturn;
}

}  // namespace mongo
