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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_current_op_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DocumentSourceCurrentOp final : public DocumentSource {
public:
    using TruncationMode = MongoProcessInterface::CurrentOpTruncateMode;
    using ConnMode = MongoProcessInterface::CurrentOpConnectionsMode;
    using LocalOpsMode = MongoProcessInterface::CurrentOpLocalOpsMode;
    using SessionMode = MongoProcessInterface::CurrentOpSessionsMode;
    using UserMode = MongoProcessInterface::CurrentOpUserMode;
    using CursorMode = MongoProcessInterface::CurrentOpCursorMode;

    static constexpr StringData kStageName = "$currentOp"_sd;

    static constexpr ConnMode kDefaultConnMode = ConnMode::kExcludeIdle;
    static constexpr SessionMode kDefaultSessionMode = SessionMode::kIncludeIdle;
    static constexpr UserMode kDefaultUserMode = UserMode::kExcludeOthers;
    static constexpr LocalOpsMode kDefaultLocalOpsMode = LocalOpsMode::kRemoteShardOps;
    static constexpr TruncationMode kDefaultTruncationMode = TruncationMode::kNoTruncation;
    static constexpr CursorMode kDefaultCursorMode = CursorMode::kExcludeCursors;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        LiteParsed(std::string parseTimeName,
                   const boost::optional<TenantId>& tenantId,
                   UserMode allUsers,
                   LocalOpsMode localOps)
            : LiteParsedDocumentSource(std::move(parseTimeName)),
              _allUsers(allUsers),
              _localOps(localOps),
              _privileges(
                  {Privilege(ResourcePattern::forClusterResource(tenantId), ActionType::inprog)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            // In a sharded cluster, we always need the inprog privilege to run $currentOp on the
            // shards. If we are only looking up local mongoS operations, we do not need inprog to
            // view our own ops but *do* require it to view other users' ops.
            if (_allUsers == UserMode::kIncludeAll ||
                (isMongos && _localOps == LocalOpsMode::kRemoteShardOps)) {
                return _privileges;
            }

            return PrivilegeVector();
        }

        bool requiresAuthzChecks() const override {
            return false;
        }

        bool isInitialSource() const final {
            return true;
        }

        bool isExemptFromIngressAdmissionControl() const final {
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return onlyReadConcernLocalSupported(kStageName, level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

    private:
        const UserMode _allUsers;
        const LocalOpsMode _localOps;
        const PrivilegeVector _privileges;
    };

    static boost::intrusive_ptr<DocumentSourceCurrentOp> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        boost::optional<ConnMode> includeIdleConnections = boost::none,
        boost::optional<SessionMode> includeIdleSessions = boost::none,
        boost::optional<UserMode> includeOpsFromAllUsers = boost::none,
        boost::optional<LocalOpsMode> showLocalOpsOnMongoS = boost::none,
        boost::optional<TruncationMode> truncateOps = boost::none,
        boost::optional<CursorMode> idleCursors = boost::none,
        boost::optional<bool> targetAllNodes = boost::none);

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        bool showLocalOps =
            _showLocalOpsOnMongoS.value_or(kDefaultLocalOpsMode) == LocalOpsMode::kLocalMongosOps;
        HostTypeRequirement hostTypeRequirement;
        if (showLocalOps) {
            hostTypeRequirement = HostTypeRequirement::kLocalOnly;
        } else if (_targetAllNodes.value_or(false)) {
            hostTypeRequirement = HostTypeRequirement::kAllShardHosts;
        } else {
            hostTypeRequirement = HostTypeRequirement::kAnyShard;
        }
        StageConstraints constraints(
            StreamType::kStreaming,
            PositionRequirement::kFirst,
            hostTypeRequirement,
            DiskUseRequirement::kNoDiskUse,
            FacetRequirement::kNotAllowed,
            TransactionRequirement::kNotAllowed,
            LookupRequirement::kAllowed,
            (showLocalOps ? UnionRequirement::kNotAllowed : UnionRequirement::kAllowed));

        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceCurrentOpToStageFn(
        const boost::intrusive_ptr<const DocumentSource>& documentSource);

    DocumentSourceCurrentOp(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                            boost::optional<ConnMode> includeIdleConnections,
                            boost::optional<SessionMode> includeIdleSessions,
                            boost::optional<UserMode> includeOpsFromAllUsers,
                            boost::optional<LocalOpsMode> showLocalOpsOnMongoS,
                            boost::optional<TruncationMode> truncateOps,
                            boost::optional<CursorMode> idleCursors,
                            boost::optional<bool> targetAllNodes)
        : DocumentSource(kStageName, pExpCtx),
          _includeIdleConnections(includeIdleConnections),
          _includeIdleSessions(includeIdleSessions),
          _includeOpsFromAllUsers(includeOpsFromAllUsers),
          _truncateOps(truncateOps),
          _idleCursors(idleCursors),
          _showLocalOpsOnMongoS(showLocalOpsOnMongoS),
          _targetAllNodes(targetAllNodes) {}

    // shared with exec::agg::CurrentOpStage
    boost::optional<ConnMode> _includeIdleConnections;
    boost::optional<SessionMode> _includeIdleSessions;
    boost::optional<UserMode> _includeOpsFromAllUsers;
    boost::optional<TruncationMode> _truncateOps;
    boost::optional<CursorMode> _idleCursors;

    boost::optional<LocalOpsMode> _showLocalOpsOnMongoS;
    boost::optional<bool> _targetAllNodes;
};

}  // namespace mongo
