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


#include "mongo/db/pipeline/document_source_current_op.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
const StringData kAllUsersFieldName = "allUsers"_sd;
const StringData kIdleConnectionsFieldName = "idleConnections"_sd;
const StringData kIdleSessionsFieldName = "idleSessions"_sd;
const StringData kLocalOpsFieldName = "localOps"_sd;
const StringData kTruncateOpsFieldName = "truncateOps"_sd;
const StringData kIdleCursorsFieldName = "idleCursors"_sd;
const StringData kBacktraceFieldName = "backtrace"_sd;
const StringData kTargetAllNodesFieldName = "targetAllNodes"_sd;

const StringData kOpIdFieldName = "opid"_sd;
const StringData kClientFieldName = "client"_sd;
const StringData kMongosClientFieldName = "client_s"_sd;
const StringData kShardFieldName = "shard"_sd;
}  // namespace

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(currentOp,
                         DocumentSourceCurrentOp::LiteParsed::parse,
                         DocumentSourceCurrentOp::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
ALLOCATE_DOCUMENT_SOURCE_ID(currentOp, DocumentSourceCurrentOp::id)

constexpr StringData DocumentSourceCurrentOp::kStageName;

std::unique_ptr<DocumentSourceCurrentOp::LiteParsed> DocumentSourceCurrentOp::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    // Need to check the value of allUsers; if true then the inprog privilege is returned by
    // requiredPrivileges(), which is called in the auth subsystem.
    if (spec.type() != BSONType::object) {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "$currentOp options must be specified in an object, but found: "
                                << typeName(spec.type()));
    }

    auto allUsers = kDefaultUserMode;
    auto localOps = kDefaultLocalOpsMode;

    // Check the spec for all fields named 'allUsers'. If any of them are 'true', we require
    // the 'inprog' privilege. This avoids the possibility that a spec with multiple
    // allUsers fields might allow an unauthorized user to view all operations. We also check for
    // the presence of a 'localOps' field, which instructs this $currentOp to list local mongoS
    // operations rather than forwarding the request to the shards.
    for (auto&& elem : spec.embeddedObject()) {
        if (elem.fieldNameStringData() == "allUsers"_sd) {
            if (elem.type() != BSONType::boolean) {
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
                    elem.type() == BSONType::boolean);

            if (elem.boolean()) {
                localOps = LocalOpsMode::kLocalMongosOps;
            }
        }
    }

    return std::make_unique<DocumentSourceCurrentOp::LiteParsed>(
        spec.fieldName(), nss.tenantId(), allUsers, localOps);
}

const char* DocumentSourceCurrentOp::getSourceName() const {
    return kStageName.data();
}

intrusive_ptr<DocumentSource> DocumentSourceCurrentOp::createFromBson(
    BSONElement spec, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$currentOp options must be specified in an object, but found: "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    const NamespaceString& nss = pExpCtx->getNamespaceString();

    uassert(ErrorCodes::InvalidNamespace,
            "$currentOp must be run against the 'admin' database with {aggregate: 1}",
            nss.isAdminDB() && nss.isCollectionlessAggregateNS());

    boost::optional<ConnMode> includeIdleConnections;
    boost::optional<SessionMode> includeIdleSessions;
    boost::optional<UserMode> includeOpsFromAllUsers;
    boost::optional<LocalOpsMode> showLocalOpsOnMongoS;
    boost::optional<TruncationMode> truncateOps;
    boost::optional<CursorMode> idleCursors;
    boost::optional<bool> targetAllNodes;

    auto currentOpSpec = CurrentOpSpec::parse(spec.embeddedObject(), IDLParserContext(kStageName));

    // Populate the values, if present.
    if (currentOpSpec.getAllUsers().has_value()) {
        includeOpsFromAllUsers = currentOpSpec.getAllUsers().value_or(false)
            ? UserMode::kIncludeAll
            : UserMode::kExcludeOthers;
    }
    if (currentOpSpec.getIdleConnections().has_value()) {
        includeIdleConnections = currentOpSpec.getIdleConnections().value_or(false)
            ? ConnMode::kIncludeIdle
            : ConnMode::kExcludeIdle;
    }
    if (currentOpSpec.getIdleCursors().has_value()) {
        idleCursors = currentOpSpec.getIdleCursors().value_or(false) ? CursorMode::kIncludeCursors
                                                                     : CursorMode::kExcludeCursors;
    }
    if (currentOpSpec.getIdleSessions().has_value()) {
        includeIdleSessions = currentOpSpec.getIdleSessions().value_or(false)
            ? SessionMode::kIncludeIdle
            : SessionMode::kExcludeIdle;
    }
    if (currentOpSpec.getLocalOps().has_value()) {
        const auto localOpsVal = currentOpSpec.getLocalOps().value_or(false);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "The 'localOps' parameter of the $currentOp stage cannot be "
                                 "true when 'targetAllNodes' is also true",
                !(targetAllNodes.value_or(false) && localOpsVal));

        showLocalOpsOnMongoS =
            localOpsVal ? LocalOpsMode::kLocalMongosOps : LocalOpsMode::kRemoteShardOps;
    }
    if (currentOpSpec.getTargetAllNodes().has_value()) {
        const auto targetAllNodesVal = currentOpSpec.getTargetAllNodes().value_or(false);
        uassert(ErrorCodes::FailedToParse,
                "The 'localOps' parameter of the $currentOp stage cannot be "
                "true when 'targetAllNodes' is also true",
                !((showLocalOpsOnMongoS &&
                   showLocalOpsOnMongoS.value() == LocalOpsMode::kLocalMongosOps) &&
                  targetAllNodesVal));

        targetAllNodes = targetAllNodesVal;

        if (targetAllNodesVal) {
            uassert(ErrorCodes::FailedToParse,
                    "$currentOp supports targetAllNodes parameter only for sharded clusters",
                    pExpCtx->getFromRouter() || pExpCtx->getInRouter());
        }
    }
    if (currentOpSpec.getTruncateOps().has_value()) {
        truncateOps = currentOpSpec.getTruncateOps().value_or(false)
            ? TruncationMode::kTruncateOps
            : TruncationMode::kNoTruncation;
    }

    return new DocumentSourceCurrentOp(pExpCtx,
                                       includeIdleConnections,
                                       includeIdleSessions,
                                       includeOpsFromAllUsers,
                                       showLocalOpsOnMongoS,
                                       truncateOps,
                                       idleCursors,
                                       targetAllNodes);
}

intrusive_ptr<DocumentSourceCurrentOp> DocumentSourceCurrentOp::create(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    boost::optional<ConnMode> includeIdleConnections,
    boost::optional<SessionMode> includeIdleSessions,
    boost::optional<UserMode> includeOpsFromAllUsers,
    boost::optional<LocalOpsMode> showLocalOpsOnMongoS,
    boost::optional<TruncationMode> truncateOps,
    boost::optional<CursorMode> idleCursors,
    boost::optional<bool> targetAllNodes) {
    return new DocumentSourceCurrentOp(pExpCtx,
                                       includeIdleConnections,
                                       includeIdleSessions,
                                       includeOpsFromAllUsers,
                                       showLocalOpsOnMongoS,
                                       truncateOps,
                                       idleCursors,
                                       targetAllNodes);
}

Value DocumentSourceCurrentOp::serialize(const SerializationOptions& opts) const {
    return Value(Document{
        {getSourceName(),
         Document{
             {kIdleConnectionsFieldName,
              _includeIdleConnections.has_value()
                  ? opts.serializeLiteral(_includeIdleConnections.value() == ConnMode::kIncludeIdle)
                  : Value()},
             {kIdleSessionsFieldName,
              _includeIdleSessions.has_value()
                  ? opts.serializeLiteral(_includeIdleSessions.value() == SessionMode::kIncludeIdle)
                  : Value()},
             {kAllUsersFieldName,
              _includeOpsFromAllUsers.has_value()
                  ? opts.serializeLiteral(_includeOpsFromAllUsers.value() == UserMode::kIncludeAll)
                  : Value()},
             {kLocalOpsFieldName,
              _showLocalOpsOnMongoS.has_value()
                  ? opts.serializeLiteral(_showLocalOpsOnMongoS.value() ==
                                          LocalOpsMode::kLocalMongosOps)
                  : Value()},
             {kTruncateOpsFieldName,
              _truncateOps.has_value()
                  ? opts.serializeLiteral(_truncateOps.value() == TruncationMode::kTruncateOps)
                  : Value()},
             {kIdleCursorsFieldName,
              _idleCursors.has_value()
                  ? opts.serializeLiteral(_idleCursors.value() == CursorMode::kIncludeCursors)
                  : Value()},
             {kTargetAllNodesFieldName,
              _targetAllNodes.has_value() ? opts.serializeLiteral(_targetAllNodes.value())
                                          : Value()}}}});
}
}  // namespace mongo
