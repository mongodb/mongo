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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceInternalListCollections::DocumentSourceInternalListCollections(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageNameInternal, pExpCtx) {}

REGISTER_DOCUMENT_SOURCE(_internalListCollections,
                         DocumentSourceInternalListCollections::LiteParsed::parse,
                         DocumentSourceInternalListCollections::createFromBson,
                         AllowedWithApiStrict::kInternal);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalListCollections, DocumentSourceInternalListCollections::id)

intrusive_ptr<DocumentSource> DocumentSourceInternalListCollections::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(9525805,
            str::stream() << kStageNameInternal
                          << " must take a nested empty object but found: " << elem,
            elem.type() == BSONType::object && elem.embeddedObject().isEmpty());

    uassert(9525806,
            str::stream() << "The " << kStageNameInternal
                          << " stage must be run against a database",
            pExpCtx->getNamespaceString().isCollectionlessAggregateNS());

    return make_intrusive<DocumentSourceInternalListCollections>(pExpCtx);
}

DocumentSourceContainer::iterator DocumentSourceInternalListCollections::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
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
            _absorbedMatch->joinMatchWith(std::move(matchRelatedToDb),
                                          MatchExpression::MatchType::AND);
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
    return kStageNameInternal.data();
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

}  // namespace mongo
