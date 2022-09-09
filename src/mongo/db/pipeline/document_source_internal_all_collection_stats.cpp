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

#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"


namespace mongo {

using boost::intrusive_ptr;

DocumentSourceInternalAllCollectionStats::DocumentSourceInternalAllCollectionStats(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    DocumentSourceInternalAllCollectionStatsSpec spec)
    : DocumentSource(kStageNameInternal, pExpCtx),
      _internalAllCollectionStatsSpec(std::move(spec)) {}

REGISTER_DOCUMENT_SOURCE(_internalAllCollectionStats,
                         DocumentSourceInternalAllCollectionStats::LiteParsed::parse,
                         DocumentSourceInternalAllCollectionStats::createFromBsonInternal,
                         AllowedWithApiStrict::kAlways);

PrivilegeVector DocumentSourceInternalAllCollectionStats::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {

    // TODO: SERVER-68249

    return PrivilegeVector{Privilege(ResourcePattern::forAnyNormalResource(), ActionType::find)};
}

DocumentSource::GetNextResult DocumentSourceInternalAllCollectionStats::doGetNext() {
    if (!_catalogDocs) {
        _catalogDocs = pExpCtx->mongoProcessInterface->listCatalog(pExpCtx->opCtx);
    }

    while (!_catalogDocs->empty()) {
        BSONObj obj(std::move(_catalogDocs->front()));
        NamespaceString nss(obj["ns"].String());

        _catalogDocs->pop_front();
        try {
            return {Document{DocumentSourceCollStats::makeStatsForNs(
                pExpCtx, nss, _internalAllCollectionStatsSpec.getStats().get())}};
        } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
            // We don't want to retrieve data for views, only for collections.
            continue;
        }
    }

    return GetNextResult::makeEOF();
}

intrusive_ptr<DocumentSource> DocumentSourceInternalAllCollectionStats::createFromBsonInternal(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6789103,
            str::stream() << "$_internalAllCollectionStats must take a nested object but found: "
                          << elem,
            elem.type() == BSONType::Object);

    uassert(6789104,
            "The $_internalAllCollectionStats stage must be run on the admin database",
            pExpCtx->ns.isAdminDB() && pExpCtx->ns.isCollectionlessAggregateNS());

    auto spec = DocumentSourceInternalAllCollectionStatsSpec::parse(
        IDLParserContext(kStageNameInternal), elem.embeddedObject());

    return make_intrusive<DocumentSourceInternalAllCollectionStats>(pExpCtx, std::move(spec));
}

const char* DocumentSourceInternalAllCollectionStats::getSourceName() const {
    return kStageNameInternal.rawData();
}

Value DocumentSourceInternalAllCollectionStats::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{getSourceName(), _internalAllCollectionStatsSpec.toBSON()}});
}
}  // namespace mongo
