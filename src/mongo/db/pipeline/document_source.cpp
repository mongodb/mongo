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

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/string_map.h"

namespace mongo {

using Parser = DocumentSource::Parser;
using boost::intrusive_ptr;
using std::list;
using std::string;
using std::vector;

DocumentSource::DocumentSource(const StringData stageName,
                               const intrusive_ptr<ExpressionContext>& pCtx)
    : pSource(nullptr), pExpCtx(pCtx), _commonStats(stageName.rawData()) {}

namespace {
// Used to keep track of which DocumentSources are registered under which name.
static StringMap<Parser> parserMap;
}  // namespace

void DocumentSource::registerParser(string name, Parser parser) {
    auto it = parserMap.find(name);
    massert(28707,
            str::stream() << "Duplicate document source (" << name << ") registered.",
            it == parserMap.end());
    parserMap[name] = parser;
}

list<intrusive_ptr<DocumentSource>> DocumentSource::parse(
    const intrusive_ptr<ExpressionContext>& expCtx, BSONObj stageObj) {
    uassert(16435,
            "A pipeline stage specification object must contain exactly one field.",
            stageObj.nFields() == 1);
    BSONElement stageSpec = stageObj.firstElement();
    auto stageName = stageSpec.fieldNameStringData();

    // Get the registered parser and call that.
    auto it = parserMap.find(stageName);

    uassert(16436,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    return it->second(stageSpec, expCtx);
}

DocumentSource::GetNextResult DocumentSource::getNext() {
    pExpCtx->checkForInterrupt();
    invariant(pExpCtx->opCtx->getServiceContext());
    invariant(pExpCtx->opCtx->getServiceContext()->getFastClockSource());
    ScopedTimer timer(pExpCtx->opCtx->getServiceContext()->getFastClockSource(),
                      &_commonStats.executionTimeMillis);
    ++_commonStats.works;

    GetNextResult next = doGetNext();
    if (next.isAdvanced()) {
        ++_commonStats.advanced;
    }
    return next;
}

const char* DocumentSource::getSourceName() const {
    static const char unknown[] = "[UNKNOWN]";
    return unknown;
}

intrusive_ptr<DocumentSource> DocumentSource::optimize() {
    return this;
}

namespace {

/**
 * Returns a pair of pointers to $match stages, either of which can be null. The first entry in the
 * pair is a $match stage that can be moved before this stage, the second is a $match stage that
 * must remain after this stage.
 */
std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
splitMatchByModifiedFields(const boost::intrusive_ptr<DocumentSourceMatch>& match,
                           const DocumentSource::GetModPathsReturn& modifiedPathsRet) {
    // Attempt to move some or all of this $match before this stage.
    std::set<std::string> modifiedPaths;
    switch (modifiedPathsRet.type) {
        case DocumentSource::GetModPathsReturn::Type::kNotSupported:
            // We don't know what paths this stage might modify, so refrain from swapping.
            return {nullptr, match};
        case DocumentSource::GetModPathsReturn::Type::kAllPaths:
            // This stage modifies all paths, so cannot be swapped with a $match at all.
            return {nullptr, match};
        case DocumentSource::GetModPathsReturn::Type::kFiniteSet:
            modifiedPaths = std::move(modifiedPathsRet.paths);
            break;
        case DocumentSource::GetModPathsReturn::Type::kAllExcept: {
            DepsTracker depsTracker;
            match->getDependencies(&depsTracker);

            auto preservedPaths = modifiedPathsRet.paths;
            for (auto&& rename : modifiedPathsRet.renames) {
                preservedPaths.insert(rename.first);
            }
            modifiedPaths =
                semantic_analysis::extractModifiedDependencies(depsTracker.fields, preservedPaths);
        }
    }
    return match->splitSourceBy(modifiedPaths, modifiedPathsRet.renames);
}

/**
 * Verifies whether or not a $group is able to swap with a succeeding $match stage. While ordinarily
 * $group can swap with a $match, it cannot if the following $match has an $exists predicate on _id,
 * and the $group has exactly one field as the $group key.  This is because every document will have
 * an _id field following such a $group stage, including those whose group key was missing before
 * the $group. As an example, the following optimization would be incorrect as the post-optimization
 * pipeline would handle documents that had nullish _id fields differently. Thus, given such a
 * $group and $match, this function would return false.
 *   {$group: {_id: "$x"}}
 *   {$match: {_id: {$exists: true}}
 * ---->
 *   {$match: {x: {$exists: true}}
 *   {$group: {_id: "$x"}}
 */
bool groupMatchSwapVerified(const DocumentSourceMatch& nextMatch,
                            const DocumentSourceGroup& thisGroup) {
    if (thisGroup.getIdFields().size() != 1) {
        return true;
    }
    return !expression::hasExistencePredicateOnPath(*(nextMatch.getMatchExpression()), "_id"_sd);
}

}  // namespace

bool DocumentSource::pushMatchBefore(Pipeline::SourceContainer::iterator itr,
                                     Pipeline::SourceContainer* container) {
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());
    auto thisGroup = dynamic_cast<DocumentSourceGroup*>(this);
    if (constraints().canSwapWithMatch && nextMatch && !nextMatch->isTextQuery() &&
        (!thisGroup || groupMatchSwapVerified(*nextMatch, *thisGroup))) {
        // We're allowed to swap with a $match and the stage after us is a $match. Furthermore, the
        // $match does not contain a text search predicate, which we do not attempt to optimize
        // because such a $match must already be the first stage in the pipeline. We can attempt to
        // swap the $match or part of the $match before ourselves.
        auto splitMatch = splitMatchByModifiedFields(nextMatch, getModifiedPaths());
        invariant(splitMatch.first || splitMatch.second);

        if (splitMatch.first) {
            // At least part of the $match can be moved before this stage. Erase the original $match
            // and put the independent part before this stage. If splitMatch.second is not null,
            // then there is a new $match stage to insert after ourselves which is dependent on the
            // modified fields.
            container->erase(std::next(itr));
            container->insert(itr, std::move(splitMatch.first));
            if (splitMatch.second) {
                container->insert(std::next(itr), std::move(splitMatch.second));
            }

            return true;
        }
    }
    return false;
}

bool DocumentSource::pushSampleBefore(Pipeline::SourceContainer::iterator itr,
                                      Pipeline::SourceContainer* container) {
    auto nextSample = dynamic_cast<DocumentSourceSample*>((*std::next(itr)).get());
    if (constraints().canSwapWithLimitAndSample && nextSample) {

        container->insert(itr, std::move(nextSample));
        container->erase(std::next(itr));

        return true;
    }
    return false;
}

Pipeline::SourceContainer::iterator DocumentSource::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    // Attempt to swap 'itr' with a subsequent $match or subsequent $sample.
    if (std::next(itr) != container->end() &&
        (pushMatchBefore(itr, container) || pushSampleBefore(itr, container))) {
        // The stage before the pushed before stage may be able to optimize further, if there is
        // such a stage.
        return std::prev(itr) == container->begin() ? std::prev(itr) : std::prev(std::prev(itr));
    }

    return doOptimizeAt(itr, container);
}

void DocumentSource::serializeToArray(vector<Value>& array,
                                      boost::optional<ExplainOptions::Verbosity> explain) const {
    Value entry = serialize(explain);
    if (!entry.missing()) {
        array.push_back(entry);
    }
}

}  // namespace mongo
