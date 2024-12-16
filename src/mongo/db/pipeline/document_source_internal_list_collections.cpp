/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_list_collections.h"

#include <boost/smart_ptr.hpp>
#include <iterator>
#include <list>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceInternalListCollections::DocumentSourceInternalListCollections(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageNameInternal, pExpCtx) {}

REGISTER_DOCUMENT_SOURCE(_internalListCollections,
                         DocumentSourceInternalListCollections::LiteParsed::parse,
                         DocumentSourceInternalListCollections::createFromBson,
                         AllowedWithApiStrict::kInternal);

DocumentSource::GetNextResult DocumentSourceInternalListCollections::doGetNext() {
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

        if (_databases->back().isInternalDb() ||
            !AuthorizationSession::get(pExpCtx->getOperationContext()->getClient())
                 ->isAuthorizedForAnyActionOnAnyResourceInDB(_databases->back())) {
            _databases->pop_back();
            continue;
        }

        _buildCollectionsToReplyForDb(_databases->back(), _collectionsToReply);
        _databases->pop_back();
    }

    auto collObj = _collectionsToReply.back().getOwned();
    _collectionsToReply.pop_back();

    return Document{collObj.getOwned()};
}

intrusive_ptr<DocumentSource> DocumentSourceInternalListCollections::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(9525805,
            str::stream() << kStageNameInternal
                          << " must take a nested empty object but found: " << elem,
            elem.type() == BSONType::Object && elem.embeddedObject().isEmpty());

    uassert(9525806,
            str::stream() << "The " << kStageNameInternal
                          << " stage must be run against a database",
            pExpCtx->getNamespaceString().isCollectionlessAggregateNS());

    return make_intrusive<DocumentSourceInternalListCollections>(pExpCtx);
}

Pipeline::SourceContainer::iterator DocumentSourceInternalListCollections::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    tassert(9741503, "The given iterator must be this object.", *itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());
    if (!nextMatch) {
        return std::next(itr);
    }

    // Get the portion of the $match that is dependent on 'db'. This will be the second attribute of
    // `splitMatch`.
    auto [matchNotRelatedToDb, matchRelatedToDb] = std::move(*nextMatch).splitSourceBy({"db"}, {});

    // Remove the original $match. We'll reintroduce the independent part of $match later.
    container->erase(std::next(itr));

    // Absorb the part of $match that is dependant on 'db'
    if (matchRelatedToDb) {
        if (!_absorbedMatch) {
            _absorbedMatch = std::move(matchRelatedToDb);
        } else {
            // We have already absorbed a $match. We need to join it.
            _absorbedMatch->joinMatchWith(std::move(matchRelatedToDb), "$and"_sd);
        }
    }

    // Put the independent part of the $match back on the pipeline.
    if (matchNotRelatedToDb) {
        container->insert(std::next(itr), std::move(matchNotRelatedToDb));
        return std::next(itr);
    } else {
        // There may be further optimization between this stage and the new neighbor, so we return
        // an iterator pointing to ourself.
        return itr;
    }
}

const char* DocumentSourceInternalListCollections::getSourceName() const {
    return kStageNameInternal.rawData();
}

void DocumentSourceInternalListCollections::serializeToArray(
    std::vector<Value>& array, const SerializationOptions& opts) const {
    auto explain = opts.verbosity;
    if (explain) {
        BSONObjBuilder bob;
        if (_absorbedMatch) {
            bob.append("match", _absorbedMatch->getQuery());
        }
        array.push_back(Value(Document{{getSourceName(), bob.obj()}}));
    } else {
        array.push_back(Value(Document{{getSourceName(), BSONObj()}}));
        if (_absorbedMatch) {
            _absorbedMatch->serializeToArray(array);
        }
    }
}

void DocumentSourceInternalListCollections::_buildCollectionsToReplyForDb(
    const DatabaseName& db, std::vector<BSONObj>& collectionsToReply) {
    tassert(9525807, "The vector 'collectionsToReply' must be empty", collectionsToReply.empty());

    const auto& dbNameStr = DatabaseNameUtil::serialize(db, SerializationContext::stateDefault());

    // Avoid computing a database that doesn't match the absorbed filter on the 'db' field.
    if (_absorbedMatch &&
        !_absorbedMatch->getMatchExpression()->matchesBSON(BSON("db" << dbNameStr))) {
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

}  // namespace mongo
