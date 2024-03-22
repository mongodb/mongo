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

#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/semantic_analysis.h"

namespace mongo {

constexpr StringData DocumentSourceGroup::kStageName;

REGISTER_DOCUMENT_SOURCE(group,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGroup::createFromBson,
                         AllowedWithApiStrict::kAlways);

const char* DocumentSourceGroup::getSourceName() const {
    return kStageName.rawData();
}

boost::intrusive_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    std::vector<AccumulationStatement> accumulationStatements,
    boost::optional<int64_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceGroup> groupStage =
        new DocumentSourceGroup(expCtx, maxMemoryUsageBytes);
    groupStage->_groupProcessor.setIdExpression(groupByExpression);
    for (auto&& statement : accumulationStatements) {
        groupStage->_groupProcessor.addAccumulationStatement(statement);
    }

    return groupStage;
}

DocumentSourceGroup::DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         boost::optional<int64_t> maxMemoryUsageBytes)
    : DocumentSourceGroupBase(kStageName, expCtx, maxMemoryUsageBytes), _groupsReady(false) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return createFromBsonWithMaxMemoryUsage(std::move(elem), expCtx, boost::none);
}

Pipeline::SourceContainer::iterator DocumentSourceGroup::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (pushDotRenamedMatch(itr, container)) {
        return itr;
    }

    return std::next(itr);
}

bool DocumentSourceGroup::pushDotRenamedMatch(Pipeline::SourceContainer::iterator itr,
                                              Pipeline::SourceContainer* container) {
    if (std::next(itr) == container->end() || std::next(std::next(itr)) == container->end()) {
        return false;
    }

    // Keep separate iterators for each stage (projection, match).
    auto prospectiveProjectionItr = std::next(itr);
    auto prospectiveProjection =
        dynamic_cast<DocumentSourceSingleDocumentTransformation*>(prospectiveProjectionItr->get());

    auto prospectiveMatchItr = std::next(std::next(itr));
    auto prospectiveMatch = dynamic_cast<DocumentSourceMatch*>(prospectiveMatchItr->get());

    if (!prospectiveProjection || !prospectiveMatch) {
        return false;
    }

    stdx::unordered_set<std::string> groupingFields;
    StringMap<std::string> relevantRenames;

    auto itsGroup = dynamic_cast<DocumentSourceGroup*>(itr->get());

    auto idFields = itsGroup->getIdFields();
    for (auto& idFieldsItr : idFields) {
        groupingFields.insert(idFieldsItr.first);
    }

    GetModPathsReturn paths = prospectiveProjection->getModifiedPaths();

    for (const auto& thisComplexRename : paths.complexRenames) {

        // Check if the dotted renaming is done on a grouping field.
        // This ensures that the top level is flat i.e., no arrays.
        if (groupingFields.find(thisComplexRename.second) != groupingFields.end()) {
            relevantRenames.insert(std::pair<std::string, std::string>(thisComplexRename.first,
                                                                       thisComplexRename.second));
        }
    }

    // Perform all changes on a copy of the match source.
    boost::intrusive_ptr<DocumentSource> currentMatchCopyDocument =
        prospectiveMatch->clone(prospectiveMatch->getContext());

    auto currentMatchCopyDocumentMatch =
        dynamic_cast<DocumentSourceMatch*>(currentMatchCopyDocument.get());

    paths.renames = std::move(relevantRenames);

    // Translate predicate statements based on the projection renames.
    auto matchSplitForProject = currentMatchCopyDocumentMatch->splitMatchByModifiedFields(
        currentMatchCopyDocumentMatch, paths);

    if (matchSplitForProject.first) {
        // Perform the swap of the projection and the match stages.
        container->erase(prospectiveMatchItr);
        container->insert(prospectiveProjectionItr, std::move(matchSplitForProject.first));

        if (matchSplitForProject.second) {
            // If there is a portion of the match stage predicate that is conflicting with the
            // projection, re-insert it below the projection stage.
            container->insert(std::next(prospectiveProjectionItr),
                              std::move(matchSplitForProject.second));
        }

        return true;
    }

    return false;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBsonWithMaxMemoryUsage(
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceGroup> groupStage(
        new DocumentSourceGroup(expCtx, maxMemoryUsageBytes));
    groupStage->initializeFromBson(elem);
    return groupStage;
}

DocumentSource::GetNextResult DocumentSourceGroup::doGetNext() {
    if (!_groupsReady) {
        auto initializationResult = performBlockingGroup();
        if (initializationResult.isPaused()) {
            return initializationResult;
        }
        invariant(initializationResult.isEOF());
    }

    auto result = _groupProcessor.getNext();
    if (!result) {
        dispose();
        return GetNextResult::makeEOF();
    }
    return GetNextResult(std::move(*result));
}

DocumentSource::GetNextResult DocumentSourceGroup::performBlockingGroup() {
    GetNextResult input = pSource->getNext();
    return performBlockingGroupSelf(input);
}

// This separate NOINLINE function is used here to decrease stack utilization of
// performBlockingGroup() and prevent stack overflows.
MONGO_COMPILER_NOINLINE DocumentSource::GetNextResult DocumentSourceGroup::performBlockingGroupSelf(
    GetNextResult input) {
    _groupProcessor.setExecutionStarted();
    // Barring any pausing, this loop exhausts 'pSource' and populates '_groups'.
    for (; input.isAdvanced(); input = pSource->getNext()) {
        // We release the result document here so that it does not outlive the end of this loop
        // iteration. Not releasing could lead to an array copy when this group follows an unwind.
        auto rootDocument = input.releaseDocument();
        Value groupKey = _groupProcessor.computeGroupKey(rootDocument);
        _groupProcessor.add(groupKey, rootDocument);
    }

    switch (input.getStatus()) {
        case DocumentSource::GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kPauseExecution: {
            return input;  // Propagate pause.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kEOF: {
            _groupProcessor.readyGroups();
            // This must happen last so that, unless control gets here, we will re-enter
            // initialization after getting a GetNextResult::ResultState::kPauseExecution.
            _groupsReady = true;
            return input;
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
