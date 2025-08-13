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

#include "mongo/db/matcher/expression_text.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
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

    std::vector<const IndexDescriptor*> idxMatches;
    collectionPtr->getIndexCatalog()->findIndexByType(opCtx, IndexNames::TEXT, idxMatches);

    uassert(ErrorCodes::IndexNotFound, "text index required for $text query", !idxMatches.empty());
    uassert(ErrorCodes::IndexNotFound,
            "more than one text index found for $text query",
            idxMatches.size() < 2);
    invariant(idxMatches.size() == 1);

    const IndexDescriptor* index = idxMatches[0];
    const FTSAccessMethod* fam = static_cast<const FTSAccessMethod*>(
        collectionPtr->getIndexCatalog()->getEntry(index)->accessMethod());
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
