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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_plan_cache_stats.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(planCacheStats,
                         DocumentSourcePlanCacheStats::LiteParsed::parse,
                         DocumentSourcePlanCacheStats::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

boost::intrusive_ptr<DocumentSource> DocumentSourcePlanCacheStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::Object);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " parameters object must be empty. Found: " << typeName(spec.type()),
            spec.embeddedObject().isEmpty());

    return new DocumentSourcePlanCacheStats(pExpCtx);
}

DocumentSourcePlanCacheStats::DocumentSourcePlanCacheStats(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {}

void DocumentSourcePlanCacheStats::serializeToArray(std::vector<Value>& array,
                                                    SerializationOptions opts) const {
    if (opts.verbosity) {
        tassert(7513100,
                "$planCacheStats is not equipped to serialize in explain mode with redaction on",
                !opts.redactIdentifiers && !opts.replacementForLiteralArgs);
        array.push_back(Value{
            Document{{kStageName,
                      Document{{"match"_sd,
                                _absorbedMatch ? Value{_absorbedMatch->getQuery()} : Value{}}}}}});
    } else {
        array.push_back(Value{Document{{kStageName, Document{}}}});
        if (_absorbedMatch) {
            _absorbedMatch->serializeToArray(array, opts);
        }
    }
}

Pipeline::SourceContainer::iterator DocumentSourcePlanCacheStats::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    auto itrToNext = std::next(itr);
    if (itrToNext == container->end()) {
        return itrToNext;
    }

    auto subsequentMatch = dynamic_cast<DocumentSourceMatch*>(itrToNext->get());
    if (!subsequentMatch) {
        return itrToNext;
    }

    _absorbedMatch = subsequentMatch;
    return container->erase(itrToNext);
}

DocumentSource::GetNextResult DocumentSourcePlanCacheStats::doGetNext() {
    if (!_haveRetrievedStats) {
        const auto matchExpr = _absorbedMatch ? _absorbedMatch->getMatchExpression() : nullptr;
        _results = pExpCtx->mongoProcessInterface->getMatchingPlanCacheEntryStats(
            pExpCtx->opCtx, pExpCtx->ns, matchExpr);

        _resultsIter = _results.begin();
        _haveRetrievedStats = true;
    }

    if (_resultsIter == _results.end()) {
        return GetNextResult::makeEOF();
    }

    MutableDocument nextPlanCacheEntry{Document{*_resultsIter++}};

    // Augment each plan cache entry with this node's host and port string.
    if (_hostAndPort.empty()) {
        _hostAndPort = pExpCtx->mongoProcessInterface->getHostAndPort(pExpCtx->opCtx);
        uassert(31386,
                "Aggregation request specified 'fromMongos' but unable to retrieve host name "
                "for $planCacheStats pipeline stage.",
                !_hostAndPort.empty());
    }
    nextPlanCacheEntry.setField("host", Value{_hostAndPort});

    // If we're returning results to mongos, then additionally augment each plan cache entry with
    // the shard name, for the node from which we're collecting plan cache information.
    if (pExpCtx->fromMongos) {
        if (_shardName.empty()) {
            _shardName = pExpCtx->mongoProcessInterface->getShardName(pExpCtx->opCtx);
            uassert(31385,
                    "Aggregation request specified 'fromMongos' but unable to retrieve shard name "
                    "for $planCacheStats pipeline stage.",
                    !_shardName.empty());
        }
        nextPlanCacheEntry.setField("shard", Value{_shardName});
    }

    return nextPlanCacheEntry.freeze();
}

}  // namespace mongo
