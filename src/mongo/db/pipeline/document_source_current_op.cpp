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

#include "mongo/db/pipeline/document_source_current_op.h"

#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

namespace {
const StringData kAllUsersFieldName = "allUsers"_sd;
const StringData kIdleConnectionsFieldName = "idleConnections"_sd;
const StringData kIdleSessionsFieldName = "idleSessions"_sd;
const StringData kLocalOpsFieldName = "localOps"_sd;
const StringData kTruncateOpsFieldName = "truncateOps"_sd;
const StringData kIdleCursorsFieldName = "idleCursors"_sd;
const StringData kBacktraceFieldName = "backtrace"_sd;

const StringData kOpIdFieldName = "opid"_sd;
const StringData kClientFieldName = "client"_sd;
const StringData kMongosClientFieldName = "client_s"_sd;
const StringData kShardFieldName = "shard"_sd;
}  // namespace

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(currentOp,
                         DocumentSourceCurrentOp::LiteParsed::parse,
                         DocumentSourceCurrentOp::createFromBson);

constexpr StringData DocumentSourceCurrentOp::kStageName;

std::unique_ptr<DocumentSourceCurrentOp::LiteParsed> DocumentSourceCurrentOp::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {
    // Need to check the value of allUsers; if true then inprog privilege is required.
    if (spec.type() != BSONType::Object) {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "$currentOp options must be specified in an object, but found: "
                                << typeName(spec.type()));
    }

    auto allUsers = UserMode::kExcludeOthers;
    auto localOps = LocalOpsMode::kRemoteShardOps;

    // Check the spec for all fields named 'allUsers'. If any of them are 'true', we require
    // the 'inprog' privilege. This avoids the possibility that a spec with multiple
    // allUsers fields might allow an unauthorized user to view all operations. We also check for
    // the presence of a 'localOps' field, which instructs this $currentOp to list local mongoS
    // operations rather than forwarding the request to the shards.
    for (auto&& elem : spec.embeddedObject()) {
        if (elem.fieldNameStringData() == "allUsers"_sd) {
            if (elem.type() != BSONType::Bool) {
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream() << "The 'allUsers' parameter of the $currentOp stage "
                                           "must be a boolean value, but found: "
                                        << typeName(elem.type()));
            }

            if (elem.boolean()) {
                allUsers = UserMode::kIncludeAll;
            }
        } else if (elem.fieldNameStringData() == "localOps") {
            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "The 'localOps' parameter of the $currentOp stage must be a "
                                     "boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);

            if (elem.boolean()) {
                localOps = LocalOpsMode::kLocalMongosOps;
            }
        }
    }

    return std::make_unique<DocumentSourceCurrentOp::LiteParsed>(allUsers, localOps);
}

const char* DocumentSourceCurrentOp::getSourceName() const {
    return kStageName.rawData();
}

DocumentSource::GetNextResult DocumentSourceCurrentOp::doGetNext() {
    if (_ops.empty()) {
        _ops = pExpCtx->mongoProcessInterface->getCurrentOps(pExpCtx,
                                                             _includeIdleConnections,
                                                             _includeIdleSessions,
                                                             _includeOpsFromAllUsers,
                                                             _truncateOps,
                                                             _idleCursors,
                                                             _backtrace);

        _opsIter = _ops.begin();

        if (pExpCtx->fromMongos) {
            _shardName = pExpCtx->mongoProcessInterface->getShardName(pExpCtx->opCtx);

            uassert(40465,
                    "Aggregation request specified 'fromMongos' but unable to retrieve shard name "
                    "for $currentOp pipeline stage.",
                    !_shardName.empty());
        }
    }

    if (_opsIter != _ops.end()) {
        if (!pExpCtx->fromMongos) {
            return Document(*_opsIter++);
        }

        // This $currentOp is running in a sharded context.
        invariant(!_shardName.empty());

        const BSONObj& op = *_opsIter++;
        MutableDocument doc;

        // Add the shard name to the output document.
        doc.addField(kShardFieldName, Value(_shardName));

        // For operations on a shard, we change the opid from the raw numeric form to
        // 'shardname:opid'. We also change the fieldname 'client' to 'client_s' to indicate
        // that the IP is that of the mongos which initiated this request.
        for (auto&& elt : op) {
            StringData fieldName = elt.fieldNameStringData();

            if (fieldName == kOpIdFieldName) {
                uassert(ErrorCodes::TypeMismatch,
                        str::stream() << "expected numeric opid for $currentOp response from '"
                                      << _shardName << "' but got: " << typeName(elt.type()),
                        elt.isNumber());

                std::string shardOpID = (str::stream() << _shardName << ":" << elt.numberInt());
                doc.addField(kOpIdFieldName, Value(shardOpID));
            } else if (fieldName == kClientFieldName) {
                doc.addField(kMongosClientFieldName, Value(elt.str()));
            } else {
                doc.addField(fieldName, Value(elt));
            }
        }

        return doc.freeze();
    }

    return GetNextResult::makeEOF();
}

intrusive_ptr<DocumentSource> DocumentSourceCurrentOp::createFromBson(
    BSONElement spec, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$currentOp options must be specified in an object, but found: "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    const NamespaceString& nss = pExpCtx->ns;

    uassert(ErrorCodes::InvalidNamespace,
            "$currentOp must be run against the 'admin' database with {aggregate: 1}",
            nss.db() == NamespaceString::kAdminDb && nss.isCollectionlessAggregateNS());

    ConnMode includeIdleConnections = ConnMode::kExcludeIdle;
    SessionMode includeIdleSessions = SessionMode::kIncludeIdle;
    UserMode includeOpsFromAllUsers = UserMode::kExcludeOthers;
    LocalOpsMode showLocalOpsOnMongoS = LocalOpsMode::kRemoteShardOps;
    TruncationMode truncateOps = TruncationMode::kNoTruncation;
    CursorMode idleCursors = CursorMode::kExcludeCursors;
    BacktraceMode backtrace = BacktraceMode::kExcludeBacktrace;

    for (auto&& elem : spec.embeddedObject()) {
        const auto fieldName = elem.fieldNameStringData();

        if (fieldName == kIdleConnectionsFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'idleConnections' parameter of the $currentOp stage must "
                                     "be a boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            includeIdleConnections =
                (elem.boolean() ? ConnMode::kIncludeIdle : ConnMode::kExcludeIdle);
        } else if (fieldName == kIdleSessionsFieldName) {
            uassert(
                ErrorCodes::FailedToParse,
                str::stream() << "The 'idleSessions' parameter of the $currentOp stage must be a "
                                 "boolean value, but found: "
                              << typeName(elem.type()),
                elem.type() == BSONType::Bool);
            includeIdleSessions =
                (elem.boolean() ? SessionMode::kIncludeIdle : SessionMode::kExcludeIdle);
        } else if (fieldName == kAllUsersFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'allUsers' parameter of the $currentOp stage must be a "
                                     "boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            includeOpsFromAllUsers =
                (elem.boolean() ? UserMode::kIncludeAll : UserMode::kExcludeOthers);
        } else if (fieldName == kLocalOpsFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'localOps' parameter of the $currentOp stage must be "
                                     "a boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            showLocalOpsOnMongoS =
                (elem.boolean() ? LocalOpsMode::kLocalMongosOps : LocalOpsMode::kRemoteShardOps);
        } else if (fieldName == kTruncateOpsFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'truncateOps' parameter of the $currentOp stage must be "
                                     "a boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            truncateOps =
                (elem.boolean() ? TruncationMode::kTruncateOps : TruncationMode::kNoTruncation);
        } else if (fieldName == kIdleCursorsFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'idleCursors' parameter of the $currentOp stage must be "
                                     "a boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            idleCursors =
                (elem.boolean() ? CursorMode::kIncludeCursors : CursorMode::kExcludeCursors);
        } else if (fieldName == kBacktraceFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'backtrace' parameter of the $currentOp stage must be "
                                     "a boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            backtrace = (elem.boolean() ? BacktraceMode::kIncludeBacktrace
                                        : BacktraceMode::kExcludeBacktrace);
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Unrecognized option '" << fieldName << "' in $currentOp stage.");
        }
    }

    return new DocumentSourceCurrentOp(pExpCtx,
                                       includeIdleConnections,
                                       includeIdleSessions,
                                       includeOpsFromAllUsers,
                                       showLocalOpsOnMongoS,
                                       truncateOps,
                                       idleCursors,
                                       backtrace);
}

intrusive_ptr<DocumentSourceCurrentOp> DocumentSourceCurrentOp::create(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    ConnMode includeIdleConnections,
    SessionMode includeIdleSessions,
    UserMode includeOpsFromAllUsers,
    LocalOpsMode showLocalOpsOnMongoS,
    TruncationMode truncateOps,
    CursorMode idleCursors,
    BacktraceMode backtrace) {
    return new DocumentSourceCurrentOp(pExpCtx,
                                       includeIdleConnections,
                                       includeIdleSessions,
                                       includeOpsFromAllUsers,
                                       showLocalOpsOnMongoS,
                                       truncateOps,
                                       idleCursors,
                                       backtrace);
}

Value DocumentSourceCurrentOp::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{
        {getSourceName(),
         Document{{kIdleConnectionsFieldName,
                   _includeIdleConnections == ConnMode::kIncludeIdle ? Value(true) : Value()},
                  {kIdleSessionsFieldName,
                   _includeIdleSessions == SessionMode::kExcludeIdle ? Value(false) : Value()},
                  {kAllUsersFieldName,
                   _includeOpsFromAllUsers == UserMode::kIncludeAll ? Value(true) : Value()},
                  {kLocalOpsFieldName,
                   _showLocalOpsOnMongoS == LocalOpsMode::kLocalMongosOps ? Value(true) : Value()},
                  {kTruncateOpsFieldName,
                   _truncateOps == TruncationMode::kTruncateOps ? Value(true) : Value()},
                  {kIdleCursorsFieldName,
                   _idleCursors == CursorMode::kIncludeCursors ? Value(true) : Value()},
                  {kBacktraceFieldName,
                   _backtrace == BacktraceMode::kIncludeBacktrace ? Value(true) : Value()}}}});
}
}  // namespace mongo
