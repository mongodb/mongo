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
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_sessions_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ListSessionsSpec listSessionsParseSpec(StringData stageName, const BSONElement& spec);
PrivilegeVector listSessionsRequiredPrivileges(const ListSessionsSpec& spec,
                                               const boost::optional<TenantId>& tenantId);
std::vector<SHA256Block> listSessionsUsersToDigests(const std::vector<ListSessionsUser>& users);

/**
 * Produces one document per session in the local cache if 'allUsers' is specified
 * as true, and returns just sessions for the currently logged in user if
 * 'allUsers' is specified as false, or not specified at all.
 */
class DocumentSourceListLocalSessions final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$listLocalSessions"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {

            return std::make_unique<LiteParsed>(
                spec.fieldName(),
                nss.tenantId(),
                listSessionsParseSpec(DocumentSourceListLocalSessions::kStageName, spec));
        }

        explicit LiteParsed(std::string parseTimeName,
                            const boost::optional<TenantId>& tenantId,
                            const ListSessionsSpec& spec)
            : LiteParsedDocumentSource(std::move(parseTimeName)),
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

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return onlyReadConcernLocalSupported(kStageName, level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(DocumentSourceListLocalSessions::kStageName);
        }

    private:
        const ListSessionsSpec _spec;
        const PrivilegeVector _privileges;
    };

    const char* getSourceName() const final {
        return DocumentSourceListLocalSessions::kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final {
        return Value(Document{{getSourceName(), _spec.toBSON(opts)}});
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kLocalOnly,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kNotAllowed);

        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    const ListSessionsSpec& getSpec() const {
        return _spec;
    }

private:
    DocumentSourceListLocalSessions(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                    const ListSessionsSpec& spec);
    const ListSessionsSpec _spec;
};

}  // namespace mongo
