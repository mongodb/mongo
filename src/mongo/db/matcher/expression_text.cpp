// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_text.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

namespace {
const FTSAccessMethod* validateFTSIndex(OperationContext* opCtx, const NamespaceString& nss) {
    auto collection = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest(
            nss,
            // TODO SERVER-93918 We shouldn't need to be careful about passing the right metadata
            // here, since this is expected to be a recursive lock acquisition in most cases. We are
            // only trying to safely grab the index metadata. Callers are expected to validate that
            // we obtain the correct shard version, storage snapshot, etc. with their own lock
            // acquisitions.
            PlacementConcern::kPretendUnsharded,
            repl::ReadConcernArgs::get(opCtx),
            AcquisitionPrerequisites::kRead));

    uassert(ErrorCodes::IndexNotFound,
            str::stream() << "text index required for $text query (no such collection '"
                          << nss.toStringForErrorMsg() << "')",
            collection.isCollection());

    const auto& collectionPtr = collection.getCollectionPtr();

    uassert(ErrorCodes::IndexNotFound,
            str::stream() << "text index required for $text query (no such collection '"
                          << nss.toStringForErrorMsg() << "')",
            collectionPtr);

    std::vector<const IndexCatalogEntry*> idxMatches;
    collectionPtr->getIndexCatalog()->findIndexByType(opCtx, IndexNames::TEXT, idxMatches);

    uassert(ErrorCodes::IndexNotFound, "text index required for $text query", !idxMatches.empty());
    uassert(ErrorCodes::IndexNotFound,
            "more than one text index found for $text query",
            idxMatches.size() < 2);

    const auto* index = idxMatches[0];
    const FTSAccessMethod* fam = static_cast<const FTSAccessMethod*>(index->accessMethod());
    invariant(fam);
    return fam;
}
}  // namespace

TextMatchExpression::TextMatchExpression(fts::FTSQueryImpl ftsQuery)
    : TextMatchExpressionBase("_fts"), _ftsQuery(ftsQuery) {}

TextMatchExpression::TextMatchExpression(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         TextParams params)
    : TextMatchExpressionBase("_fts") {
    _ftsQuery.setQuery(std::move(params.query));
    _ftsQuery.setLanguage(std::move(params.language));
    _ftsQuery.setCaseSensitive(params.caseSensitive);
    _ftsQuery.setDiacriticSensitive(params.diacriticSensitive);

    fts::TextIndexVersion version;
    {
        const FTSAccessMethod* fam = validateFTSIndex(opCtx, nss);

        // Extract version and default language from text index.
        version = fam->getSpec().getTextIndexVersion();
        if (_ftsQuery.getLanguage().empty()) {
            _ftsQuery.setLanguage(fam->getSpec().defaultLanguage().str());
        }
    }

    Status parseStatus = _ftsQuery.parse(version);
    uassertStatusOK(parseStatus);
}

std::unique_ptr<MatchExpression> TextMatchExpression::clone() const {
    auto expr = std::make_unique<TextMatchExpression>(_ftsQuery);
    // We use the query-only constructor here directly rather than using the full constructor, to
    // avoid needing to examine
    // the index catalog.
    if (getTag()) {
        expr->setTag(getTag()->clone());
    }
    return expr;
}

}  // namespace mongo
