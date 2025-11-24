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

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/string_map.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using boost::intrusive_ptr;

StringMap<DocumentSource::ParserRegistration> DocumentSource::parserMap;

DocumentSource::DocumentSource(StringData stageName, const intrusive_ptr<ExpressionContext>& pCtx)
    : _expCtx(pCtx) {}

void DocumentSource::unregisterParser_forTest(const std::string& name) {
    parserMap.erase(name);
}

void DocumentSource::registerParser(std::string name,
                                    Parser parser,
                                    FeatureFlag* featureFlag,
                                    bool skipIfExists) {
    // Set of aggregation stages that are allowed to be overridden (via extensions).
    static const stdx::unordered_set<StringData> allowedOverrideStages = {
        DocumentSourceVectorSearch::kStageName,
    };

    auto it = parserMap.find(name);
    if (skipIfExists && it != parserMap.end()) {
        // Short-circuit if the stage is already registered and skipIfExists is true.
        LOGV2_DEBUG(10918508,
                    2,
                    "Silently skipping registration of parser since name already exists",
                    "name"_attr = name);
        return;
    }

    // Allow override only for stages in the allowed list, otherwise assert on duplicates.
    if (it != parserMap.end() && !allowedOverrideStages.contains(name)) {
        // Parser registration only takes place during startup, so any issues with parser
        // registration should fail startup completely. For clarity, that is why we use fassert
        // (shuts down the whole process) instead of tassert (fails an individual operation),
        // although a tassert would technically fail the process anyways.
        LOGV2_FATAL(28707, "Cannot register duplicate aggregation stage.", "stageName"_attr = name);
    }

    parserMap[std::move(name)] = {std::move(parser), featureFlag};
}

void DocumentSource::registerParser(std::string name,
                                    SimpleParser simpleParser,
                                    FeatureFlag* featureFlag,
                                    bool skipIfExists) {
    Parser parser = [simpleParser = std::move(simpleParser)](
                        BSONElement stageSpec, const intrusive_ptr<ExpressionContext>& expCtx)
        -> std::list<intrusive_ptr<DocumentSource>> {
        return {simpleParser(std::move(stageSpec), expCtx)};
    };
    return registerParser(std::move(name), std::move(parser), std::move(featureFlag), skipIfExists);
}

DocumentSource::Id DocumentSource::allocateId(StringData name) {
    static AtomicWord<Id> next{kUnallocatedId + 1};
    auto id = next.fetchAndAdd(1);
    LOGV2_DEBUG(9901900, 5, "Allocating DocumentSourceId", "id"_attr = id, "name"_attr = name);
    return id;
}

bool DocumentSource::hasQuery() const {
    return false;
}

BSONObj DocumentSource::getQuery() const {
    MONGO_UNREACHABLE;
}

std::list<intrusive_ptr<DocumentSource>> DocumentSource::parse(
    const intrusive_ptr<ExpressionContext>& expCtx, BSONObj stageObj) {
    uassert(16435,
            "A pipeline stage specification object must contain exactly one field.",
            stageObj.nFields() == 1);
    BSONElement stageSpec = stageObj.firstElement();
    auto stageName = stageSpec.fieldNameStringData();

    return parseCommon(expCtx, stageSpec, stageName);
}

std::list<intrusive_ptr<DocumentSource>> DocumentSource::parseFromLiteParsed(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const LiteParsedDocumentSource& liteParsed) {
    return parseCommon(expCtx, liteParsed.getOriginalBson(), liteParsed.getParseTimeName());
}

std::list<intrusive_ptr<DocumentSource>> DocumentSource::parseCommon(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement stageSpec,
    StringData stageName) {
    // Get the registered parser and call that.
    auto it = parserMap.find(stageName);

    uassert(16436,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    auto& entry = it->second;
    if (entry.featureFlag) {
        expCtx->ignoreFeatureInParserOrRejectAndThrow(stageName, *entry.featureFlag);
    }

    return it->second.parser(stageSpec, expCtx);
}

BSONObj DocumentSource::serializeToBSONForDebug() const {
    std::vector<Value> serialized;
    auto opts = SerializationOptions{
        .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
    serializeToArray(serialized, opts);
    if (serialized.empty()) {
        LOGV2_DEBUG(5943501,
                    5,
                    "warning: stage did not serialize to anything as it was trying to be printed "
                    "for debugging");
        return BSONObj();
    }
    if (serialized.size() > 1) {
        LOGV2_DEBUG(5943502, 5, "stage serialized to multiple stages. Ignoring all but the first");
    }
    return serialized[0].getDocument().toBson();
}

void DocumentSource::serializeToArray(std::vector<Value>& array,
                                      const SerializationOptions& opts) const {
    Value entry = serialize(opts);
    if (!entry.missing()) {
        array.push_back(std::move(entry));
    }
}

MONGO_INITIALIZER_GROUP(BeginDocumentSourceRegistration,
                        ("default"),
                        ("EndDocumentSourceRegistration"))
MONGO_INITIALIZER_GROUP(EndDocumentSourceRegistration, ("BeginDocumentSourceRegistration"), ())
MONGO_INITIALIZER_GROUP(BeginDocumentSourceIdAllocation,
                        ("default"),
                        ("EndDocumentSourceIdAllocation"))
MONGO_INITIALIZER_GROUP(EndDocumentSourceIdAllocation, ("BeginDocumentSourceIdAllocation"), ())
}  // namespace mongo
