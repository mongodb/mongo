// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_list_collections_stage.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/document_source_internal_list_collections.h"
#include "mongo/db/shard_role/ddl/list_collections_gen.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalListCollectionsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* internalListCollectionsDS =
        dynamic_cast<DocumentSourceInternalListCollections*>(documentSource.get());

    tassert(10812300,
            "expected 'DocumentSourceInternalListCollections' type",
            internalListCollectionsDS);

    return make_intrusive<exec::agg::InternalListCollectionsStage>(
        internalListCollectionsDS->kStageName,
        internalListCollectionsDS->getExpCtx(),
        internalListCollectionsDS->_absorbedMatch);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalListCollections,
                           DocumentSourceInternalListCollections::id,
                           documentSourceInternalListCollectionsToStageFn)

InternalListCollectionsStage::InternalListCollectionsStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<DocumentSourceMatch>& absorbedMatch)
    : Stage(stageName, pExpCtx), _absorbedMatch(absorbedMatch) {}

GetNextResult InternalListCollectionsStage::doGetNext() {
    if (!_databases) {
        if (pExpCtx->getNamespaceString().isAdminDB()) {
            _databases = pExpCtx->getMongoProcessInterface()->getAllDatabases(
                pExpCtx->getOperationContext(), pExpCtx->getNamespaceString().tenantId());
        } else {
            _databases = std::vector<DatabaseName>{pExpCtx->getNamespaceString().dbName()};
        }
    }

    while (_collectionsToReply.empty()) {
        if (_databases->empty()) {
            return GetNextResult::makeEOF();
        }

        _buildCollectionsToReplyForDb(_databases->back(), _collectionsToReply);
        _databases->pop_back();
    }

    auto collObj = _collectionsToReply.back().getOwned();
    _collectionsToReply.pop_back();

    return Document{collObj.getOwned()};
}

void InternalListCollectionsStage::_buildCollectionsToReplyForDb(
    const DatabaseName& db, std::vector<BSONObj>& collectionsToReply) {
    tassert(9525807, "The vector 'collectionsToReply' must be empty", collectionsToReply.empty());

    const auto& dbNameStr = DatabaseNameUtil::serialize(db, SerializationContext::stateDefault());

    // Avoid computing a database that doesn't match the absorbed filter on the 'db' field.
    if (_absorbedMatch &&
        !exec::matcher::matchesBSON(_absorbedMatch->getMatchExpression(),
                                    BSON("db" << dbNameStr))) {
        return;
    }

    const auto& collectionsList = pExpCtx->getMongoProcessInterface()->runListCollections(
        pExpCtx->getOperationContext(), db, true /* addPrimaryShard */);
    collectionsToReply.reserve(collectionsList.size());

    for (const auto& collObj : collectionsList) {
        BSONObjBuilder builder;

        // Removing the 'name' field and adding a 'ns' field to return the entire namespace of
        // the collection instead of just the collection name.
        //
        // In addition, we are adding the 'db' field to make it easier for the user to filter by
        // this field.
        for (const auto& elem : collObj) {
            auto fieldName = elem.fieldNameStringData();
            if (fieldName == ListCollectionsReplyItem::kNameFieldName) {
                const auto& collName = elem.valueStringDataSafe();
                const auto& nssStr = dbNameStr + "." + std::string{collName};
                builder.append("ns", nssStr);
                builder.append("db", dbNameStr);
            } else {
                builder.append(elem);
            }
        }

        const auto obj = builder.obj();
        collectionsToReply.push_back(obj.getOwned());
    }
}

}  // namespace exec::agg
}  // namespace mongo
