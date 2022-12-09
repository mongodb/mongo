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

#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/util/destructor_guard.h"

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
    boost::optional<size_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceGroup> groupStage =
        new DocumentSourceGroup(expCtx, maxMemoryUsageBytes);
    groupStage->setIdExpression(groupByExpression);
    for (auto&& statement : accumulationStatements) {
        groupStage->addAccumulator(statement);
    }

    return groupStage;
}

DocumentSourceGroup::DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         boost::optional<size_t> maxMemoryUsageBytes)
    : DocumentSourceGroupBase(kStageName, expCtx, maxMemoryUsageBytes), _groupsReady(false) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return createFromBsonWithMaxMemoryUsage(std::move(elem), expCtx, boost::none);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBsonWithMaxMemoryUsage(
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<size_t> maxMemoryUsageBytes) {
    boost::intrusive_ptr<DocumentSourceGroup> groupStage(
        new DocumentSourceGroup(expCtx, maxMemoryUsageBytes));
    groupStage->initializeFromBson(elem);
    return groupStage;
}

DocumentSource::GetNextResult DocumentSourceGroup::doGetNext() {
    if (!_groupsReady) {
        const auto initializationResult = performBlockingGroup();
        if (initializationResult.isPaused()) {
            return initializationResult;
        }
        invariant(initializationResult.isEOF());
    }

    auto result = getNextReadyGroup();
    if (result.isEOF()) {
        dispose();
    }
    return result;
}

DocumentSource::GetNextResult DocumentSourceGroup::performBlockingGroup() {
    GetNextResult input = pSource->getNext();
    return performBlockingGroupSelf(input);
}

// This separate NOINLINE function is used here to decrease stack utilization of
// performBlockingGroup() and prevent stack overflows.
MONGO_COMPILER_NOINLINE DocumentSource::GetNextResult DocumentSourceGroup::performBlockingGroupSelf(
    GetNextResult input) {
    setExecutionStarted();
    // Barring any pausing, this loop exhausts 'pSource' and populates '_groups'.
    for (; input.isAdvanced(); input = pSource->getNext()) {
        if (shouldSpillWithAttemptToSaveMemory()) {
            spill();
        }

        // We release the result document here so that it does not outlive the end of this loop
        // iteration. Not releasing could lead to an array copy when this group follows an unwind.
        auto rootDocument = input.releaseDocument();
        Value id = computeId(rootDocument);
        processDocument(id, rootDocument);
    }

    switch (input.getStatus()) {
        case DocumentSource::GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kPauseExecution: {
            return input;  // Propagate pause.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kEOF: {
            readyGroups();
            // This must happen last so that, unless control gets here, we will re-enter
            // initialization after getting a GetNextResult::ResultState::kPauseExecution.
            _groupsReady = true;
            return input;
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
