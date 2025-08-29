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

#include "mongo/db/pipeline/document_source_plan_cache_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/str.h"

#include <iterator>
#include <list>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE(planCacheStats,
                         DocumentSourcePlanCacheStats::LiteParsed::parse,
                         DocumentSourcePlanCacheStats::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
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
        uassert(4503200,
                "$planCacheStats stage supports allHosts parameter only for sharded clusters",
                pExpCtx->getFromRouter() || pExpCtx->getInRouter());
    }
    return new DocumentSourcePlanCacheStats(pExpCtx, allHosts);
}

DocumentSourcePlanCacheStats::DocumentSourcePlanCacheStats(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool allHosts)
    : DocumentSource(kStageName, expCtx), _allHosts(allHosts) {}

void DocumentSourcePlanCacheStats::serializeToArray(std::vector<Value>& array,
                                                    const SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        tassert(7513100,
                "$planCacheStats is not equipped to serialize in explain mode with redaction on",
                opts.isDefaultSerialization());
        array.push_back(Value{Document{
            {kStageName,
             Document{{"match"_sd, _absorbedMatch ? Value{_absorbedMatch->getQuery()} : Value{}},
                      {"allHosts"_sd, _allHosts}}}}});
    } else {
        array.push_back(Value{Document{{kStageName, Document{{"allHosts"_sd, _allHosts}}}}});
        if (_absorbedMatch) {
            _absorbedMatch->serializeToArray(array, opts);
        }
    }
}

DocumentSourceContainer::iterator DocumentSourcePlanCacheStats::doOptimizeAt(
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
