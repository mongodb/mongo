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

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListClusterCatalog);

/**
 * This aggregation stage is the '$listClusterCatalog' stage.
 * Lists any collection in the catalog and their related sharding informations.
 */
namespace DocumentSourceListClusterCatalog {
using namespace std::literals::string_view_literals;

static constexpr std::string_view kStageName = "$listClusterCatalog"sv;

class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
public:
    static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                             const BSONElement& spec,
                                             const LiteParserOptions& options) {
        return std::make_unique<LiteParsed>(spec, nss);
    }

    LiteParsed(const BSONElement& spec, const NamespaceString& nss)
        : LiteParsedDocumentSourceDefault(spec) {

        if (nss.dbName() != DatabaseName::kAdmin) {
            if (nss.isCollectionlessAggregateNS()) {
                _privileges.emplace_back(Privilege(ResourcePattern::forDatabaseName(nss.dbName()),
                                                   ActionType::listCollections));
            } else {
                // This stage is designed to operate only on database-level namespaces (without
                // collections). By authorizing any users to run $listClusterCatalog when a
                // collection is specified, we allow the stage to fail correctly and warn the user
                // that only a database name should be provided instead of failing with
                // authorization issues.
                _privileges.emplace_back(Privilege());
            }
        } else {
            _privileges.emplace_back(Privilege(ResourcePattern::forClusterResource(nss.tenantId()),
                                               ActionType::listClusterCatalog));
        }
    };

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

    bool generatesOwnDataOnce() const final {
        return true;
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<ListClusterCatalogStageParams>(_originalBson);
    }

    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        // The listCollections command that runs under the hood only accepts 'local' read concern.
        return onlyReadConcernLocalSupported(kStageName, level, isImplicitDefault);
    }

    void assertSupportsMultiDocumentTransaction() const override {
        transactionNotSupported(kStageName);
    }

private:
    PrivilegeVector _privileges;
};

static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

};  // namespace DocumentSourceListClusterCatalog

}  // namespace mongo
