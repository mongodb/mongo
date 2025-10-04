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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
                         AllowedWithApiStrict::kInternal);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalAllCollectionStats,
                            DocumentSourceInternalAllCollectionStats::id)

DocumentSourceContainer::iterator DocumentSourceInternalAllCollectionStats::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Attempt to internalize any predicates of a $project stage in order to calculate only
    // necessary fields.
    if (auto nextProject =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>((*std::next(itr)).get())) {
        _projectFilter = nextProject->getTransformer().serializeTransformation().toBson();
    }

    // Attempt to internalize any predicates of a $match upon the "ns" field.
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    auto splitMatch = std::move(*nextMatch).splitSourceBy({"ns"}, {});
    invariant(splitMatch.first || splitMatch.second);

    // Remove the original $match.
    container->erase(std::next(itr));

    // Absorb the part of $match that is dependant on 'ns'
    if (splitMatch.second) {
        if (!_absorbedMatch) {
            _absorbedMatch = std::move(splitMatch.second);
        } else {
            // We have already absorbed a $match. We need to join it with splitMatch.second.
            _absorbedMatch->joinMatchWith(std::move(splitMatch.second),
                                          MatchExpression::MatchType::AND);
        }
    }

    // splitMatch.first is independent of 'ns'. Put it back on the pipeline.
    if (splitMatch.first) {
        container->insert(std::next(itr), std::move(splitMatch.first));
        return std::next(itr);
    } else {
        // There may be further optimization between this stage and the new neighbor, so we return
        // an iterator pointing to ourself.
        return itr;
    }
}

void DocumentSourceInternalAllCollectionStats::serializeToArray(
    std::vector<Value>& array, const SerializationOptions& opts) const {
    auto explain = opts.verbosity;
    if (explain) {
        BSONObjBuilder bob;
        _internalAllCollectionStatsSpec.serialize(&bob, opts);
        if (_absorbedMatch) {
            bob.append("match", _absorbedMatch->getQuery());
        }
        auto doc = Document{{getSourceName(), bob.obj()}};
        array.push_back(Value(doc));
    } else {
        array.push_back(serialize(opts));
        if (_absorbedMatch) {
            _absorbedMatch->serializeToArray(array);
        }
    }
}

intrusive_ptr<DocumentSource> DocumentSourceInternalAllCollectionStats::createFromBsonInternal(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6789103,
            str::stream() << "$_internalAllCollectionStats must take a nested object but found: "
                          << elem,
            elem.type() == BSONType::object);

    uassert(6789104,
            "The $_internalAllCollectionStats stage must be run on the admin database",
            pExpCtx->getNamespaceString().isAdminDB() &&
                pExpCtx->getNamespaceString().isCollectionlessAggregateNS());

    auto spec = DocumentSourceInternalAllCollectionStatsSpec::parse(
        elem.embeddedObject(), IDLParserContext(kStageNameInternal));

    return make_intrusive<DocumentSourceInternalAllCollectionStats>(pExpCtx, std::move(spec));
}

const char* DocumentSourceInternalAllCollectionStats::getSourceName() const {
    return kStageNameInternal.data();
}

Value DocumentSourceInternalAllCollectionStats::serialize(const SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _internalAllCollectionStatsSpec.toBSON(opts)}});
}
}  // namespace mongo
