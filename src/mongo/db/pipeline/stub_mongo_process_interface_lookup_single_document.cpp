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

#include "mongo/db/pipeline/stub_mongo_process_interface_lookup_single_document.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/util/assert_util.h"

namespace mongo {

std::unique_ptr<Pipeline, PipelineDeleter>
StubMongoProcessInterfaceLookupSingleDocument::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions opts) {
    auto pipeline = uassertStatusOK(Pipeline::parse(rawPipeline, expCtx));

    if (opts.optimize) {
        pipeline->optimizePipeline();
    }

    if (opts.attachCursorSource) {
        pipeline = attachCursorSourceToPipeline(expCtx, pipeline.release());
    }

    return pipeline;
}

std::unique_ptr<Pipeline, PipelineDeleter>
StubMongoProcessInterfaceLookupSingleDocument::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline) {
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));
    pipeline->addInitialSource(DocumentSourceMock::create(_mockResults));
    return pipeline;
}

boost::optional<Document> StubMongoProcessInterfaceLookupSingleDocument::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern,
    bool allowSpeculativeMajorityRead) {
    // The namespace 'nss' may be different than the namespace on the ExpressionContext in the
    // case of a change stream on a whole database so we need to make a copy of the
    // ExpressionContext with the new namespace.
    auto foreignExpCtx = expCtx->copyWith(nss, collectionUUID, boost::none);
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        pipeline = makePipeline({BSON("$match" << documentKey)}, foreignExpCtx);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = pipeline->getNext();
    if (auto next = pipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document matching "
                                << documentKey.toString()
                                << " ["
                                << lookedUpDocument->toString()
                                << ", "
                                << next->toString()
                                << "]");
    }
    return lookedUpDocument;
}

}  // namespace mongo
