// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;
using QueryShapeConfigurationMap = stdx::unordered_map<query_shape::QueryShapeHash,
                                                       query_settings::QueryShapeConfiguration,
                                                       QueryShapeHashHasher>;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(QuerySettings);

/**
 * The $querySettings stage returns all QueryShapeConfigurations stored in the cluster. It fully
 * desugars at parse time into a pipeline that joins the in-memory configurations with their
 * representative queries.
 */
class DocumentSourceQuerySettings final {
public:
    static constexpr std::string_view kStageName{"$querySettings"};

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            uassert(7746800,
                    "$querySettings stage expects a document as argument",
                    spec.type() == BSONType::object);
            return std::make_unique<LiteParsed>(spec, nss.tenantId());
        }

        std::unique_ptr<StageParams> getStageParams() const override {
            return std::make_unique<QuerySettingsStageParams>(_originalBson);
        }

        LiteParsed(const BSONElement& spec, const boost::optional<TenantId>& tenantId)
            : LiteParsedDocumentSourceDefault(spec),
              _privileges({Privilege(ResourcePattern::forClusterResource(tenantId),
                                     ActionType::querySettings)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return _privileges;
        }

        bool generatesOwnDataOnce() const final {
            return true;
        }

        bool isInitialSource() const override {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

    private:
        const PrivilegeVector _privileges;
    };

    static DocumentSourceContainer createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceQuerySettings directly, use createFromBson()
    // instead.
    DocumentSourceQuerySettings() = default;
};

}  // namespace mongo
