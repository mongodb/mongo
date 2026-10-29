// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_list_catalog.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(listCatalog,
                                     DocumentSourceListCatalog::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_CONTAINER_WITH_STAGE_PARAMS_DEFAULT(listCatalog,
                                                             DocumentSourceListCatalog,
                                                             ListCatalogStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(listCatalog, DocumentSourceListCatalog::id)

std::string_view DocumentSourceListCatalog::getSourceName() const {
    return kStageName;
}

PrivilegeVector DocumentSourceListCatalog::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {

    // Refer to privileges for the readAnyDatabase role in addReadOnlyAnyDbPrivileges().
    // See builtin_roles.cpp.
    ActionSet listCollectionsAndIndexesActions{ActionType::listCollections,
                                               ActionType::listIndexes};
    if (_ns.isCollectionlessAggregateNS()) {
        const auto& tenantId = _ns.tenantId();
        return {Privilege(ResourcePattern::forClusterResource(tenantId), ActionType::listDatabases),
                Privilege(ResourcePattern::forAnyNormalResource(tenantId),
                          listCollectionsAndIndexesActions),
                Privilege(ResourcePattern::forCollectionName(tenantId, "system.js"sv),
                          listCollectionsAndIndexesActions),
                Privilege(ResourcePattern::forAnySystemBuckets(tenantId),
                          listCollectionsAndIndexesActions)};
    } else {
        return {
            Privilege(ResourcePattern::forExactNamespace(_ns), listCollectionsAndIndexesActions)};
    }
}

DocumentSourceListCatalog::DocumentSourceListCatalog(
    const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx) {}

DocumentSourceContainer DocumentSourceListCatalog::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6200600,
            "The $listCatalog stage specification must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());

    const NamespaceString& nss = pExpCtx->getNamespaceString();

    uassert(
        ErrorCodes::InvalidNamespace,
        "Collectionless $listCatalog must be run against the 'admin' database with {aggregate: 1}",
        nss.isAdminDB() || !nss.isCollectionlessAggregateNS());

    DocumentSourceContainer result;
    result.emplace_back(new DocumentSourceListCatalog(pExpCtx));

    // For collectionless requests from non-internal callers, inject a server-owned $match that
    // hides config.*, local.*, and system.* metadata (except system.js and system.buckets.*).
    // Requests forwarded from a router to a shard have internal privileges, so the isInternal
    // check below already prevents double-injection without a separate fromRouter guard.
    //
    // TODO(SERVER-129978): remove this workaround once mongosync uses
    // listCollections/listIndexes/listDatabases instead of $listCatalog.
    if (nss.isCollectionlessAggregateNS()) {
        auto* opCtx = pExpCtx->getOperationContext();
        auto* authSession = AuthorizationSession::get(opCtx->getClient());
        const bool isInternal = authSession->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(nss.tenantId()), ActionType::internal);
        if (!isInternal) {
            // $match predicate that hides restricted catalog entries from non-internal callers.
            // The keep-set mirrors the auth privilege model for collectionless $listCatalog:
            //  - anyNormalResource (any non-system collection in any non-config/local database)
            //  - system.js
            //  - anySystemBuckets (system.buckets.*)
            // An entry is kept when BOTH:
            //  1. db is neither "config" nor "local"
            //  2. name does not start with "system.", OR is exactly "system.js",
            //     OR starts with "system.buckets."
            static const BSONObj kNamespaceFilter = fromjson(R"({
                $and: [
                    {db: {$nin: ["config", "local"]}},
                    {$or: [
                        {name: {$not: /^system\\./}},
                        {name: "system.js"},
                        {name: /^system\\.buckets\\./}
                    ]}
                ]
            })");
            result.emplace_back(DocumentSourceMatch::create(kNamespaceFilter, pExpCtx));
        }
    }

    return result;
}

Value DocumentSourceListCatalog::serialize(const query_shape::SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << Document()));
}
}  // namespace mongo
