// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_list_sessions_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListSessions);

/**
 * $listSessions: { allUsers: true/false, users: [ {user:"jsmith", db:"test"}, ... ] }
 * Return all sessions in the config.system.sessions collection
 * or just sessions for the currently logged in user. (Default: false)
 *
 * This is essentially an alias for {$match:{"_id.uid": myid}} or {$match:{}}
 * with appropriate constraints on stage positioning and security checks
 * based on ActionType::ListSessions
 */
class DocumentSourceListSessions final : public DocumentSourceMatch {
public:
    DocumentSourceListSessions(const DocumentSourceListSessions& other,
                               const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
        : DocumentSourceMatch(other, newExpCtx), _allUsers(other._allUsers), _users(other._users) {}

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        return make_intrusive<std::decay_t<decltype(*this)>>(*this, newExpCtx);
    }

    static constexpr std::string_view kStageName = "$listSessions"sv;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(
                spec,
                nss.tenantId(),
                listSessionsParseSpec(DocumentSourceListSessions::kStageName, spec));
        }

        LiteParsed(const BSONElement& specElem,
                   const boost::optional<TenantId>& tenantId,
                   const ListSessionsSpec& spec)
            : LiteParsedDocumentSourceDefault(specElem),
              _spec(spec),
              _privileges(listSessionsRequiredPrivileges(_spec, tenantId)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return _privileges;
        }

        bool requiresAuthzChecks() const override {
            return false;
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<ListSessionsStageParams>(_originalBson);
        }

    private:
        const ListSessionsSpec _spec;
        const PrivilegeVector _privileges;
    };

    std::string_view getSourceName() const final {
        return DocumentSourceListSessions::kStageName;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.consumesLogicalCollectionData = false;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceListSessions(const BSONObj& query,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                               const bool allUsers,
                               const boost::optional<std::vector<mongo::ListSessionsUser>>& users)
        : DocumentSourceMatch(query, pExpCtx), _allUsers(allUsers), _users(users) {}

    bool _allUsers;
    boost::optional<std::vector<mongo::ListSessionsUser>> _users;
};

}  // namespace mongo
