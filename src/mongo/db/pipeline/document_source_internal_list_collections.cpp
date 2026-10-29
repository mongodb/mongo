// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_list_collections.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceInternalListCollections::DocumentSourceInternalListCollections(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx) {}

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalListCollections,
                                     DocumentSourceInternalListCollections::LiteParsed::parse,
                                     AllowedWithApiStrict::kInternal);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalListCollections,
                                                   DocumentSourceInternalListCollections,
                                                   InternalListCollectionsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalListCollections, DocumentSourceInternalListCollections::id);

intrusive_ptr<DocumentSource> DocumentSourceInternalListCollections::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(9525805,
            str::stream() << kStageName << " must take a nested empty object but found: " << elem,
            elem.type() == BSONType::object && elem.embeddedObject().isEmpty());

    uassert(9525806,
            str::stream() << "The " << kStageName << " stage must be run against a database",
            pExpCtx->getNamespaceString().isCollectionlessAggregateNS());

    return make_intrusive<DocumentSourceInternalListCollections>(pExpCtx);
}

DocumentSourceContainer::iterator DocumentSourceInternalListCollections::optimizeAt(
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

std::string_view DocumentSourceInternalListCollections::getSourceName() const {
    return kStageName;
}

void DocumentSourceInternalListCollections::serializeToArray(
    std::vector<Value>& array, const query_shape::SerializationOptions& opts) const {
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
