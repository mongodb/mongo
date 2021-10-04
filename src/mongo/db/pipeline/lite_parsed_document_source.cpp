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

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

using Parser = LiteParsedDocumentSource::Parser;

namespace {

// Empty vector used by LiteParsedDocumentSources which do not have a sub pipeline.
inline static std::vector<LiteParsedPipeline> kNoSubPipeline = {};

StringMap<LiteParsedDocumentSource::LiteParserInfo> parserMap;

}  // namespace

void LiteParsedDocumentSource::registerParser(const std::string& name,
                                              Parser parser,
                                              AllowedWithApiStrict allowedWithApiStrict,
                                              AllowedWithClientType allowedWithClientType) {
    parserMap[name] = {parser, allowedWithApiStrict, allowedWithClientType};
    // Initialize a counter for this document source to track how many times it is used.
    aggStageCounters.stageCounterMap[name] = std::make_unique<AggStageCounters::StageCounter>(name);
}

std::unique_ptr<LiteParsedDocumentSource> LiteParsedDocumentSource::parse(
    const NamespaceString& nss, const BSONObj& spec) {
    uassert(40323,
            "A pipeline stage specification object must contain exactly one field.",
            spec.nFields() == 1);
    BSONElement specElem = spec.firstElement();

    auto stageName = specElem.fieldNameStringData();
    auto it = parserMap.find(stageName);

    uassert(40324,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    return it->second.parser(nss, specElem);
}

const LiteParsedDocumentSource::LiteParserInfo& LiteParsedDocumentSource::getInfo(
    const std::string& stageName) {
    auto it = parserMap.find(stageName);
    uassert(5407200,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    return it->second;
}

const std::vector<LiteParsedPipeline>& LiteParsedDocumentSource::getSubPipelines() const {
    return kNoSubPipeline;
}

LiteParsedDocumentSourceNestedPipelines::LiteParsedDocumentSourceNestedPipelines(
    std::string parseTimeName,
    boost::optional<NamespaceString> foreignNss,
    std::vector<LiteParsedPipeline> pipelines)
    : LiteParsedDocumentSource(std::move(parseTimeName)),
      _foreignNss(std::move(foreignNss)),
      _pipelines(std::move(pipelines)) {}

LiteParsedDocumentSourceNestedPipelines::LiteParsedDocumentSourceNestedPipelines(
    std::string parseTimeName,
    boost::optional<NamespaceString> foreignNss,
    boost::optional<LiteParsedPipeline> pipeline)
    : LiteParsedDocumentSourceNestedPipelines(
          std::move(parseTimeName), std::move(foreignNss), std::vector<LiteParsedPipeline>{}) {
    if (pipeline)
        _pipelines.emplace_back(std::move(pipeline.get()));
}

stdx::unordered_set<NamespaceString>
LiteParsedDocumentSourceNestedPipelines::getInvolvedNamespaces() const {
    stdx::unordered_set<NamespaceString> involvedNamespaces;
    if (_foreignNss)
        involvedNamespaces.insert(*_foreignNss);

    for (auto&& pipeline : _pipelines) {
        auto involvedInSubPipe = pipeline.getInvolvedNamespaces();
        involvedNamespaces.insert(involvedInSubPipe.begin(), involvedInSubPipe.end());
    }
    return involvedNamespaces;
}

bool LiteParsedDocumentSourceNestedPipelines::allowedToPassthroughFromMongos() const {
    // If any of the sub-pipelines doesn't allow pass through, then return false.
    return std::all_of(_pipelines.cbegin(), _pipelines.cend(), [](const auto& subPipeline) {
        return subPipeline.allowedToPassthroughFromMongos();
    });
}

bool LiteParsedDocumentSourceNestedPipelines::allowShardedForeignCollection(
    NamespaceString nss) const {
    return std::all_of(_pipelines.begin(), _pipelines.end(), [&nss](auto&& pipeline) {
        return pipeline.allowShardedForeignCollection(nss);
    });
}

ReadConcernSupportResult LiteParsedDocumentSourceNestedPipelines::supportsReadConcern(
    repl::ReadConcernLevel level, bool isImplicitDefault) const {
    // Assume that the document source holding the pipeline has no constraints of its own, so
    // return the strictest of the constraints on the sub-pipelines.
    auto result = ReadConcernSupportResult::allSupportedAndDefaultPermitted();
    for (auto& pipeline : _pipelines) {
        result.merge(pipeline.sourcesSupportReadConcern(level, isImplicitDefault));
        // If both result statuses are already not OK, stop checking.
        if (!result.readConcernSupport.isOK() && !result.defaultReadConcernPermit.isOK()) {
            break;
        }
    }
    return result;
}

}  // namespace mongo
