// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_plan_cache_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/str.h"

#include <iterator>
#include <list>

#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(planCacheStats,
                                     DocumentSourcePlanCacheStats::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(planCacheStats,
                                                   DocumentSourcePlanCacheStats,
                                                   PlanCacheStatsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(planCacheStats, DocumentSourcePlanCacheStats::id)

boost::intrusive_ptr<DocumentSource> DocumentSourcePlanCacheStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::object);

    bool allHosts = false;
    BSONObjIterator specIt(spec.embeddedObject());
    if (specIt.more()) {
        BSONElement e = specIt.next();
        auto fieldName = e.fieldNameStringData();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << kStageName
                              << " parameters object may contain only 'allHosts' field. Found: "
                              << fieldName,
                fieldName == "allHosts");
        allHosts = e.Bool();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << kStageName << " parameters object may contain at most one field.",
                !specIt.more());
    }
    if (allHosts) {
        // This check is necessary to correctly target shards. In circumstances where we aren't
        // actually running the query (e.g. parsing for query shape), we don't need to do this (and
        // it can erroneously error - SERVER-117156).
        uassert(4503200,
                "$planCacheStats stage supports allHosts parameter only for sharded clusters",
                pExpCtx->getFromRouter() || pExpCtx->getInRouter() ||
                    !pExpCtx->getMongoProcessInterface()->isExpectedToExecuteQueries());
    }
    return new DocumentSourcePlanCacheStats(pExpCtx, allHosts);
}

DocumentSourcePlanCacheStats::DocumentSourcePlanCacheStats(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool allHosts)
    : DocumentSource(kStageName, expCtx), _allHosts(allHosts) {}

void DocumentSourcePlanCacheStats::serializeToArray(
    std::vector<Value>& array, const query_shape::SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        tassert(7513100,
                "$planCacheStats is not equipped to serialize in explain mode with redaction on",
                opts.isDefaultSerialization());
        array.push_back(Value{Document{
            {kStageName,
             Document{{"match"sv, _absorbedMatch ? Value{_absorbedMatch->getQuery()} : Value{}},
                      {"allHosts"sv, _allHosts}}}}});
    } else {
        array.push_back(Value{Document{{kStageName, Document{{"allHosts"sv, _allHosts}}}}});
        if (_absorbedMatch) {
            _absorbedMatch->serializeToArray(array, opts);
        }
    }
}

DocumentSourceContainer::iterator DocumentSourcePlanCacheStats::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
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

}  // namespace mongo
