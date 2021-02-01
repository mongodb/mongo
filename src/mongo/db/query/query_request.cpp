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

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_request.h"

#include <memory>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"

namespace mongo {

QueryRequest::QueryRequest(NamespaceStringOrUUID nssOrUuid, bool preferNssForSerialization)
    : _findCommand(std::move(nssOrUuid)) {
    if (preferNssForSerialization) {
        _findCommand.getNamespaceOrUUID().preferNssForSerialization();
    }
}
QueryRequest::QueryRequest(FindCommand findCommand) : _findCommand(std::move(findCommand)) {
    _findCommand.getNamespaceOrUUID().preferNssForSerialization();
}

void QueryRequest::refreshNSS(const NamespaceString& nss) {
    if (_findCommand.getNamespaceOrUUID().uuid()) {
        auto& nssOrUUID = _findCommand.getNamespaceOrUUID();
        nssOrUUID.setNss(nss);
    }
    invariant(_findCommand.getNamespaceOrUUID().nss());
}

// static
std::unique_ptr<QueryRequest> QueryRequest::makeFromFindCommand(
    const BSONObj& cmdObj, bool isExplain, boost::optional<NamespaceString> nss, bool apiStrict) {

    auto qr = std::make_unique<QueryRequest>(
        FindCommand::parse(IDLParserErrorContext("FindCommand", apiStrict), cmdObj));

    // If there is an explicit namespace specified overwite it.
    if (nss) {
        qr->setNSS(*nss);
    }

    qr->_tailableMode =
        uassertStatusOK(tailableModeFromBools(qr->getTailable(), qr->getAwaitData()));

    qr->_explain = isExplain;
    qr->addMetaProjection();

    if (qr->getSkip() && *qr->getSkip() == 0) {
        qr->setSkip(boost::none);
    }
    if (qr->getLimit() && *qr->getLimit() == 0) {
        qr->setLimit(boost::none);
    }
    uassertStatusOK(qr->validate());
    return qr;
}

std::unique_ptr<QueryRequest> QueryRequest::makeFromFindCommandForTests(
    const BSONObj& cmdObj, bool isExplain, boost::optional<NamespaceString> nss, bool apiStrict) {
    return makeFromFindCommand(cmdObj, isExplain, nss, apiStrict);
}

BSONObj QueryRequest::asFindCommand() const {
    BSONObjBuilder bob;
    asFindCommand(&bob);
    return bob.obj();
}

void QueryRequest::asFindCommand(BSONObjBuilder* cmdBuilder) const {
    _findCommand.serialize(BSONObj(), cmdBuilder);
}

void QueryRequest::addShowRecordIdMetaProj() {
    if (getProj()["$recordId"]) {
        // There's already some projection on $recordId. Don't overwrite it.
        return;
    }

    BSONObjBuilder projBob;
    projBob.appendElements(getProj());
    BSONObj metaRecordId = BSON("$recordId" << BSON("$meta" << QueryRequest::metaRecordId));
    projBob.append(metaRecordId.firstElement());
    setProj(projBob.obj());
}

Status QueryRequest::validate() const {
    // Min and Max objects must have the same fields.
    if (!getMin().isEmpty() && !getMax().isEmpty()) {
        if (!getMin().isFieldNamePrefixOf(getMax()) || (getMin().nFields() != getMax().nFields())) {
            return Status(ErrorCodes::Error(51176), "min and max must have the same field names");
        }
    }

    if ((getLimit() || getBatchSize()) && getNToReturn()) {
        return Status(ErrorCodes::BadValue,
                      "'limit' or 'batchSize' fields can not be set with 'ntoreturn' field.");
    }

    // TODO SERVER-53060: When legacy query request is seperated, these validations can be moved to
    // IDL.
    if (getSkip() && *getSkip() < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Skip value must be non-negative, but received: " << *getSkip());
    }

    if (getLimit() && *getLimit() < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Limit value must be non-negative, but received: " << *getLimit());
    }

    if (getBatchSize() && *getBatchSize() < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "BatchSize value must be non-negative, but received: "
                                    << *getBatchSize());
    }

    if (getNToReturn() && *getNToReturn() < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "NToReturn value must be non-negative, but received: "
                                    << *getNToReturn());
    }

    if (getMaxTimeMS() < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "MaxTimeMS value must be non-negative, but received: "
                                    << getMaxTimeMS());
    }

    if (_tailableMode != TailableModeEnum::kNormal) {
        // Tailable cursors cannot have any sort other than {$natural: 1}.
        const BSONObj expectedSort = BSON(kNaturalSortField << 1);
        if (!getSort().isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(getSort() != expectedSort)) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with a sort other than {$natural: 1}");
        }

        // Cannot indicate that you want a 'singleBatch' if the cursor is tailable.
        if (isSingleBatch()) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with the 'singleBatch' option");
        }
    }

    if (getRequestResumeToken()) {
        if (SimpleBSONObjComparator::kInstance.evaluate(getHint() !=
                                                        BSON(kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "hint must be {$natural:1} if 'requestResumeToken' is enabled");
        }
        if (!getSort().isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(getSort() !=
                                                        BSON(kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "sort must be unset or {$natural:1} if 'requestResumeToken' is enabled");
        }
        if (!getResumeAfter().isEmpty()) {
            if (getResumeAfter().nFields() != 1 ||
                (getResumeAfter()["$recordId"].type() != BSONType::NumberLong &&
                 getResumeAfter()["$recordId"].type() != BSONType::jstOID &&
                 getResumeAfter()["$recordId"].type() != BSONType::jstNULL)) {
                return Status(
                    ErrorCodes::BadValue,
                    "Malformed resume token: the '_resumeAfter' object must contain"
                    " exactly one field named '$recordId', of type NumberLong, jstOID or jstNULL.");
            }
        }
    } else if (!getResumeAfter().isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      "'requestResumeToken' must be true if 'resumeAfter' is"
                      " specified");
    }
    return Status::OK();
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
    if (metaElt.fieldNameStringData() != "$meta") {
        return false;
    }
    if (mongo::String != metaElt.type()) {
        return false;
    }
    if (StringData{metaElt.valuestr()} != QueryRequest::metaTextScore) {
        return false;
    }
    // must have exactly 1 element
    if (metaIt.more()) {
        return false;
    }
    return true;
}

//
// Old QueryRequest parsing code: SOON TO BE DEPRECATED.
//

// static
StatusWith<std::unique_ptr<QueryRequest>> QueryRequest::fromLegacyQueryMessage(
    const QueryMessage& qm) {
    auto qr = std::make_unique<QueryRequest>(NamespaceString(qm.ns));

    Status status = qr->init(qm.ntoskip, qm.ntoreturn, qm.queryOptions, qm.query, qm.fields, true);
    if (!status.isOK()) {
        return status;
    }

    return std::move(qr);
}

StatusWith<std::unique_ptr<QueryRequest>> QueryRequest::fromLegacyQuery(
    NamespaceStringOrUUID nsOrUuid,
    const BSONObj& queryObj,
    const BSONObj& proj,
    int ntoskip,
    int ntoreturn,
    int queryOptions) {
    // Legacy command should prefer to serialize with UUID.
    auto qr = std::make_unique<QueryRequest>(std::move(nsOrUuid), false);

    Status status = qr->init(ntoskip, ntoreturn, queryOptions, queryObj, proj, true);
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
    if (!proj.isEmpty()) {
        _findCommand.setProjection(proj.getOwned());
    }
    if (ntoskip) {
        _findCommand.setSkip(ntoskip);
    }

    if (ntoreturn) {
        if (ntoreturn < 0) {
            if (ntoreturn == std::numeric_limits<int>::min()) {
                // ntoreturn is negative but can't be negated.
                return Status(ErrorCodes::BadValue, "bad ntoreturn value in query");
            }
            _findCommand.setNtoreturn(-ntoreturn);
            setSingleBatchField(true);
        } else {
            _findCommand.setNtoreturn(ntoreturn);
        }
    }

    // An ntoreturn of 1 is special because it also means to return at most one batch.
    if (getNToReturn().value_or(0) == 1) {
        setSingleBatchField(true);
    }

    // Initialize flags passed as 'queryOptions' bit vector.
    initFromInt(queryOptions);

    if (fromQueryMessage) {
        BSONElement queryField = queryObj["query"];
        if (!queryField.isABSONObj()) {
            queryField = queryObj["$query"];
        }
        if (queryField.isABSONObj()) {
            _findCommand.setFilter(queryField.embeddedObject().getOwned());
            Status status = initFullQuery(queryObj);
            if (!status.isOK()) {
                return status;
            }
        } else {
            _findCommand.setFilter(queryObj.getOwned());
        }
        // It's not possible to specify readConcern in a legacy query message, so initialize it to
        // an empty readConcern object, ie. equivalent to `readConcern: {}`.  This ensures that
        // mongos passes this empty readConcern to shards.
        setReadConcern(BSONObj());
    } else {
        // This is the debugging code path.
        _findCommand.setFilter(queryObj.getOwned());
    }

    _hasReadPref = queryObj.hasField("$readPreference");

    return validate();
}

Status QueryRequest::initFullQuery(const BSONObj& top) {
    BSONObjIterator i(top);

    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        if (name == "$orderby" || name == "orderby") {
            if (Object == e.type()) {
                setSort(e.embeddedObject().getOwned());
            } else if (Array == e.type()) {
                setSort(e.embeddedObject());

                // TODO: Is this ever used?  I don't think so.
                // Quote:
                // This is for languages whose "objects" are not well ordered (JSON is well
                // ordered).
                // [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
                // note: this is slow, but that is ok as order will have very few pieces
                BSONObjBuilder b;
                char p[2] = "0";

                while (1) {
                    BSONObj j = getSort().getObjectField(p);
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

                setSort(b.obj());
            } else {
                return Status(ErrorCodes::BadValue, "sort must be object or array");
            }
        } else if (name.startsWith("$")) {
            name = name.substr(1);  // chop first char
            if (name == "explain") {
                // Won't throw.
                _explain = e.trueValue();
            } else if (name == "min") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$min must be a BSONObj");
                }
                setMin(e.embeddedObject().getOwned());
            } else if (name == "max") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$max must be a BSONObj");
                }
                setMax(e.embeddedObject().getOwned());
            } else if (name == "hint") {
                if (e.isABSONObj()) {
                    setHint(e.embeddedObject().getOwned());
                } else if (String == e.type()) {
                    setHint(e.wrap());
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "$hint must be either a string or nested object");
                }
            } else if (name == "returnKey") {
                // Won't throw.
                if (e.trueValue()) {
                    setReturnKey(true);
                }
            } else if (name == "showDiskLoc") {
                // Won't throw.
                if (e.trueValue()) {
                    setShowRecordId(true);
                    addShowRecordIdMetaProj();
                }
            } else if (name == "maxTimeMS") {
                StatusWith<int> maxTimeMS = parseMaxTimeMS(e);
                if (!maxTimeMS.isOK()) {
                    return maxTimeMS.getStatus();
                }
                setMaxTimeMS(maxTimeMS.getValue());
            }
        }
    }

    return Status::OK();
}

int QueryRequest::getOptions() const {
    int options = 0;
    if (_tailableMode == TailableModeEnum::kTailable) {
        options |= QueryOption_CursorTailable;
    } else if (_tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        options |= QueryOption_CursorTailable;
        options |= QueryOption_AwaitData;
    }
    if (_slaveOk) {
        options |= QueryOption_SecondaryOk;
    }
    if (isNoCursorTimeout()) {
        options |= QueryOption_NoCursorTimeout;
    }
    if (_exhaust) {
        options |= QueryOption_Exhaust;
    }
    if (isAllowPartialResults()) {
        options |= QueryOption_PartialResults;
    }
    return options;
}

void QueryRequest::initFromInt(int options) {
    bool tailable = (options & QueryOption_CursorTailable) != 0;
    bool awaitData = (options & QueryOption_AwaitData) != 0;
    if (awaitData) {
        _findCommand.setAwaitData(true);
    }
    if (tailable) {
        _findCommand.setTailable(true);
    }
    _tailableMode = uassertStatusOK(tailableModeFromBools(tailable, awaitData));
    _slaveOk = (options & QueryOption_SecondaryOk) != 0;
    _exhaust = (options & QueryOption_Exhaust) != 0;

    if ((options & QueryOption_NoCursorTimeout) != 0) {
        setNoCursorTimeout(true);
    }
    if ((options & QueryOption_PartialResults) != 0) {
        setAllowPartialResults(true);
    }
}

void QueryRequest::addMetaProjection() {
    if (showRecordId()) {
        addShowRecordIdMetaProj();
    }
}

boost::optional<int64_t> QueryRequest::getEffectiveBatchSize() const {
    return getBatchSize() ? getBatchSize() : getNToReturn();
}

StatusWith<BSONObj> QueryRequest::asAggregationCommand() const {
    BSONObjBuilder aggregationBuilder;

    // First, check if this query has options that are not supported in aggregation.
    if (!getMin().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kMinFieldName
                              << " not supported in aggregation."};
    }
    if (!getMax().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kMaxFieldName
                              << " not supported in aggregation."};
    }
    if (returnKey()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kReturnKeyFieldName
                              << " not supported in aggregation."};
    }
    if (showRecordId()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kShowRecordIdFieldName
                              << " not supported in aggregation."};
    }
    if (isTailable()) {
        return {ErrorCodes::InvalidPipelineOperator,
                "Tailable cursors are not supported in aggregation."};
    }
    if (isNoCursorTimeout()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kNoCursorTimeoutFieldName
                              << " not supported in aggregation."};
    }
    if (isAllowPartialResults()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kAllowPartialResultsFieldName
                              << " not supported in aggregation."};
    }
    if (getNToReturn()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cannot convert to an aggregation if ntoreturn is set."};
    }
    if (getSort()[kNaturalSortField]) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Sort option " << kNaturalSortField
                              << " not supported in aggregation."};
    }
    // The aggregation command normally does not support the 'singleBatch' option, but we make a
    // special exception if 'limit' is set to 1.
    if (isSingleBatch() && getLimit().value_or(0) != 1LL) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kSingleBatchFieldName
                              << " not supported in aggregation."};
    }
    if (isReadOnce()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kReadOnceFieldName
                              << " not supported in aggregation."};
    }

    if (allowSpeculativeMajorityRead()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kAllowSpeculativeMajorityReadFieldName
                              << " not supported in aggregation."};
    }

    if (getRequestResumeToken()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kRequestResumeTokenFieldName
                              << " not supported in aggregation."};
    }

    if (!getResumeAfter().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommand::kResumeAfterFieldName
                              << " not supported in aggregation."};
    }

    // Now that we've successfully validated this QR, begin building the aggregation command.
    aggregationBuilder.append("aggregate",
                              _findCommand.getNamespaceOrUUID().nss()
                                  ? _findCommand.getNamespaceOrUUID().nss()->coll()
                                  : "");

    // Construct an aggregation pipeline that finds the equivalent documents to this query request.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    if (!getFilter().isEmpty()) {
        BSONObjBuilder matchBuilder(pipelineBuilder.subobjStart());
        matchBuilder.append("$match", getFilter());
        matchBuilder.doneFast();
    }
    if (!getSort().isEmpty()) {
        BSONObjBuilder sortBuilder(pipelineBuilder.subobjStart());
        sortBuilder.append("$sort", getSort());
        sortBuilder.doneFast();
    }
    if (getSkip()) {
        BSONObjBuilder skipBuilder(pipelineBuilder.subobjStart());
        skipBuilder.append("$skip", *getSkip());
        skipBuilder.doneFast();
    }
    if (getLimit()) {
        BSONObjBuilder limitBuilder(pipelineBuilder.subobjStart());
        limitBuilder.append("$limit", *getLimit());
        limitBuilder.doneFast();
    }
    if (!getProj().isEmpty()) {
        BSONObjBuilder projectBuilder(pipelineBuilder.subobjStart());
        projectBuilder.append("$project", getProj());
        projectBuilder.doneFast();
    }
    pipelineBuilder.doneFast();

    // The aggregation 'cursor' option is always set, regardless of the presence of batchSize.
    BSONObjBuilder batchSizeBuilder(aggregationBuilder.subobjStart("cursor"));
    if (getBatchSize()) {
        batchSizeBuilder.append(FindCommand::kBatchSizeFieldName, *getBatchSize());
    }
    batchSizeBuilder.doneFast();

    // Other options.
    aggregationBuilder.append("collation", getCollation());
    if (getMaxTimeMS() > 0) {
        aggregationBuilder.append(cmdOptionMaxTimeMS, getMaxTimeMS());
    }
    if (!getHint().isEmpty()) {
        aggregationBuilder.append(FindCommand::kHintFieldName, getHint());
    }
    if (getReadConcern()) {
        aggregationBuilder.append("readConcern", *getReadConcern());
    }
    if (!getUnwrappedReadPref().isEmpty()) {
        aggregationBuilder.append(FindCommand::kUnwrappedReadPrefFieldName, getUnwrappedReadPref());
    }
    if (allowDiskUse()) {
        aggregationBuilder.append(FindCommand::kAllowDiskUseFieldName, allowDiskUse());
    }
    if (getLegacyRuntimeConstants()) {
        BSONObjBuilder rtcBuilder(
            aggregationBuilder.subobjStart(FindCommand::kLegacyRuntimeConstantsFieldName));
        getLegacyRuntimeConstants()->serialize(&rtcBuilder);
        rtcBuilder.doneFast();
    }
    if (getLetParameters()) {
        aggregationBuilder.append(FindCommand::kLetFieldName, *getLetParameters());
    }
    return StatusWith<BSONObj>(aggregationBuilder.obj());
}
}  // namespace mongo
