/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_list_catalog.h"

#include <fmt/format.h>

#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/version/releases.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE_WITH_MIN_VERSION(listCatalog,
                                          DocumentSourceListCatalog::LiteParsed::parse,
                                          DocumentSourceListCatalog::createFromBson,
                                          AllowedWithApiStrict::kNeverInVersion1,
                                          multiversion::FeatureCompatibilityVersion::kVersion_6_0);

const char* DocumentSourceListCatalog::getSourceName() const {
    return kStageName.rawData();
}

PrivilegeVector DocumentSourceListCatalog::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {

    // Refer to privileges for the readAnyDatabase role in addReadOnlyAnyDbPrivileges().
    // See builtin_roles.cpp.
    ActionSet listCollectionsAndIndexesActions{ActionType::listCollections,
                                               ActionType::listIndexes};
    return _ns.isCollectionlessAggregateNS()
        ? PrivilegeVector{Privilege(ResourcePattern::forClusterResource(),
                                    ActionType::listDatabases),
                          Privilege(ResourcePattern::forAnyNormalResource(),
                                    listCollectionsAndIndexesActions),
                          Privilege(ResourcePattern::forCollectionName("system.js"),
                                    listCollectionsAndIndexesActions),
                          Privilege(ResourcePattern::forAnySystemBuckets(),
                                    listCollectionsAndIndexesActions)}
        : PrivilegeVector{
              Privilege(ResourcePattern::forExactNamespace(_ns), listCollectionsAndIndexesActions)};
}

DocumentSource::GetNextResult DocumentSourceListCatalog::doGetNext() {
    if (!_catalogDocs) {
        if (pExpCtx->ns.isCollectionlessAggregateNS()) {
            _catalogDocs = pExpCtx->mongoProcessInterface->listCatalog(pExpCtx->opCtx);
        } else if (auto catalogDoc = pExpCtx->mongoProcessInterface->getCatalogEntry(pExpCtx->opCtx,
                                                                                     pExpCtx->ns)) {
            _catalogDocs = {{std::move(*catalogDoc)}};
        } else {
            _catalogDocs.emplace();
        }
    }

    if (!_catalogDocs->empty()) {
        Document doc{std::move(_catalogDocs->front())};
        _catalogDocs->pop_front();
        return doc;
    }

    return GetNextResult::makeEOF();
}

DocumentSourceListCatalog::DocumentSourceListCatalog(
    const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx) {}

intrusive_ptr<DocumentSource> DocumentSourceListCatalog::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6200600,
            "The $listCatalog stage specification must be an empty object",
            elem.type() == Object && elem.Obj().isEmpty());

    const NamespaceString& nss = pExpCtx->ns;

    uassert(
        ErrorCodes::InvalidNamespace,
        "Collectionless $listCatalog must be run against the 'admin' database with {aggregate: 1}",
        nss.db() == NamespaceString::kAdminDb || !nss.isCollectionlessAggregateNS());

    uassert(ErrorCodes::QueryFeatureNotAllowed,
            fmt::format("The {} aggregation stage is not enabled", kStageName),
            feature_flags::gDocumentSourceListCatalog.isEnabled(
                serverGlobalParams.featureCompatibility));

    return new DocumentSourceListCatalog(pExpCtx);
}

Value DocumentSourceListCatalog::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(DOC(getSourceName() << Document()));
}
}  // namespace mongo
