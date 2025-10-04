/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/internal_list_collections_stage.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/pipeline/document_source_internal_list_collections.h"
#include "mongo/util/assert_util.h"

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
        internalListCollectionsDS->kStageNameInternal,
        internalListCollectionsDS->getExpCtx(),
        internalListCollectionsDS->_absorbedMatch);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalListCollections,
                           DocumentSourceInternalListCollections::id,
                           documentSourceInternalListCollectionsToStageFn)

InternalListCollectionsStage::InternalListCollectionsStage(
    StringData stageName,
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
                const auto& nssStr = dbNameStr + "." + collName;
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
