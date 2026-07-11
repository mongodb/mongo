// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListMqlEntities);

/**
 * Test-only aggregation stage which describes the set of MQL entities available in this binary,
 * which may vary depending on the binary (mongod/mongos, commmunity/enterprise). For the
 * DocumentSource MQL entity type, this stage returns only the set of stages which have a registered
 * parser and are thus user-facing via the 'aggregate' command; it does not include DocumentSources
 * which we create during desugaring or optimization. The order of results is guarenteed to be
 * sorted by name.
 */
class DocumentSourceListMqlEntities final : public DocumentSource {
public:
    class LiteParsed : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(specElem);
        }

        LiteParsed(const BSONElement& spec) : LiteParsedDocumentSourceDefault(spec) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        // No privileges required because this is a test-only stage.
        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {};
        }

        bool requiresAuthzChecks() const override {
            return false;
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<ListMqlEntitiesStageParams>(_originalBson);
        }
    };

    static constexpr std::string_view kStageName = "$listMqlEntities"sv;
    static constexpr std::string_view kEntityTypeFieldName = "entityType"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceListMqlEntities(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  MqlEntityTypeEnum type);

    StageConstraints constraints(PipelineSplitState pipeState) const final;
    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    MqlEntityTypeEnum getType() const {
        return _type;
    }

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    MqlEntityTypeEnum _type;
};

}  // namespace mongo
