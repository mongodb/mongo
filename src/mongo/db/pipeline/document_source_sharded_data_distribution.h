// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ShardedDataDistribution);

/**
 * This aggregation stage is an alias for ‘$shardedDataDistribution’. It takes no arguments. Its
 * response will be a cursor, each document of which represents the data-distribution information
 * for a particular collection.
 */
namespace DocumentSourceShardedDataDistribution {
using namespace std::literals::string_view_literals;

static constexpr std::string_view kStageName = "$shardedDataDistribution"sv;

class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
public:
    static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                             const BSONElement& spec,
                                             const LiteParserOptions& options) {
        return std::make_unique<LiteParsed>(spec, nss.tenantId());
    }

    LiteParsed(const BSONElement& spec, const boost::optional<TenantId>& tenantId)
        : LiteParsedDocumentSourceDefault(spec),
          _privileges({Privilege(ResourcePattern::forClusterResource(tenantId),
                                 ActionType::shardedDataDistribution)}) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return {NamespaceString::kConfigsvrCollectionsNamespace,
                NamespaceString::kConfigsvrChunksNamespace};
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return _privileges;
    }

    bool isInitialSource() const final {
        return true;
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<ShardedDataDistributionStageParams>(_originalBson);
    }

private:
    const PrivilegeVector _privileges;
};

static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

};  // namespace DocumentSourceShardedDataDistribution

}  // namespace mongo
