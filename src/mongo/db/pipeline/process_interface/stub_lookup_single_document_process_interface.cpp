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

#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

std::unique_ptr<Pipeline>
StubLookupSingleDocumentProcessInterface::attachCursorSourceToPipelineForLocalRead(
    Pipeline* ownedPipeline,
    boost::optional<const AggregateCommandRequest&> aggRequest,
    bool shouldUseCollectionDefaultCollator,
    ExecShardFilterPolicy shardFilterPolicy) {
    std::unique_ptr<Pipeline> pipeline(ownedPipeline);
    pipeline->addInitialSource(
        DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
    return pipeline;
}

std::unique_ptr<Pipeline> StubLookupSingleDocumentProcessInterface::preparePipelineForExecution(
    Pipeline* ownedPipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    return attachCursorSourceToPipelineForLocalRead(ownedPipeline);
}

std::unique_ptr<Pipeline> StubLookupSingleDocumentProcessInterface::preparePipelineForExecution(
    const boost::intrusive_ptr<mongo::ExpressionContext>& expCtx,
    const AggregateCommandRequest& aggRequest,
    Pipeline* pipeline,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    return attachCursorSourceToPipelineForLocalRead(
        pipeline, aggRequest, shouldUseCollectionDefaultCollator);
}

boost::optional<Document> StubLookupSingleDocumentProcessInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    // The namespace 'nss' may be different than the namespace on the ExpressionContext in the
    // case of a change stream on a whole database so we need to make a copy of the
    // ExpressionContext with the new namespace.
    auto foreignExpCtx = makeCopyFromExpressionContext(expCtx, nss, collectionUUID);
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<exec::agg::Pipeline> execPipeline;
    try {
        pipeline = Pipeline::makePipeline({BSON("$match" << documentKey)}, foreignExpCtx);
        execPipeline = exec::agg::buildPipeline(pipeline->freeze());
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = execPipeline->getNext();
    if (auto next = execPipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document matching "
                                << documentKey.toString() << " [" << lookedUpDocument->toString()
                                << ", " << next->toString() << "]");
    }
    return lookedUpDocument;
}

}  // namespace mongo
