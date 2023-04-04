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
#include "mongo/db/pipeline/document_source_project.h"

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

DocumentSource::GetNextResult DocumentSourceInternalAllCollectionStats::doGetNext() {
    if (!_catalogDocs) {
        _catalogDocs = pExpCtx->mongoProcessInterface->listCatalog(pExpCtx->opCtx);
    }

    while (!_catalogDocs->empty()) {
        BSONObj obj(std::move(_catalogDocs->front()));
        NamespaceString nss(obj["ns"].String());

        _catalogDocs->pop_front();

        // Avoid computing stats for collections that do not match the absorbed filter on the 'ns'
        // field.
        if (_absorbedMatch && !_absorbedMatch->getMatchExpression()->matchesBSON(std::move(obj))) {
            continue;
        }

        if (const auto& stats = _internalAllCollectionStatsSpec.getStats()) {
            try {
                return {Document{DocumentSourceCollStats::makeStatsForNs(
                    pExpCtx, nss, stats.get(), _projectFilter)}};
            } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
                // We don't want to retrieve data for views, only for collections.
                continue;
            }
        }
    }

    return GetNextResult::makeEOF();
}

Pipeline::SourceContainer::iterator DocumentSourceInternalAllCollectionStats::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Attempt to internalize any predicates of a $project stage in order to calculate only
    // necessary fields.
    if (auto nextProject =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>((*std::next(itr)).get())) {
        _projectFilter =
            nextProject->getTransformer().serializeTransformation(boost::none).toBson();
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
            _absorbedMatch->joinMatchWith(std::move(splitMatch.second));
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

void DocumentSourceInternalAllCollectionStats::serializeToArray(std::vector<Value>& array,
                                                                SerializationOptions opts) const {
    auto explain = opts.verbosity;
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484341);
    }
    if (explain) {
        BSONObjBuilder bob;
        _internalAllCollectionStatsSpec.serialize(&bob);
        if (_absorbedMatch) {
            bob.append("match", _absorbedMatch->getQuery());
        }
        auto doc = Document{{getSourceName(), bob.obj()}};
        array.push_back(Value(doc));
    } else {
        array.push_back(serialize(explain));
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

Value DocumentSourceInternalAllCollectionStats::serialize(SerializationOptions opts) const {
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484340);
    }
    return Value(Document{{getSourceName(), _internalAllCollectionStatsSpec.toBSON()}});
}
}  // namespace mongo
