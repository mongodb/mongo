// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/modules.h"

#include <list>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class Pipeline;
}  // namespace mongo

namespace mongo::change_stream::pipeline_helpers {

/**
 * Adds the top-level error-conversion stage ('$_internalChangeStreamConvertErrors') to an
 * opted-in ('showErrorsAsEvents: true') change stream pipeline, if it is not already present.
 *
 * Must be called after optimization and before the pipeline is frozen, so that the optimizer never
 * sees the stage while explain still reports it. No-op for non-change-stream pipelines and when the
 * flag is off, and idempotent if called more than once.
 */
void addErrorConversionStageIfNeeded(Pipeline& pipeline);

/**
 * Build a change stream pipeline for the config server, used by v2 change streams.
 */
std::vector<BSONObj> buildPipelineForConfigServerV2(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    OperationContext* opCtx,
    Timestamp atClusterTime,
    const NamespaceString& nss,
    const ChangeStream& changeStream,
    ChangeStreamReaderBuilder* readerBuilder);

/**
 * Build a change stream pipeline for the router or a data shard, depending on the settings in the
 * ExpressionContext. Calling this function can modify change stream-specific settings in the
 * ExpressionContext.
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec,
    const ResumeTokenData& resumeToken);

/**
 * Returns a copy of the user-defined single-document transformation stages ('$match', '$redact',
 * '$project', '$addFields', ...) that execute on the shards, in pipeline order. The returned stages
 * are used to re-apply the user pipeline to the events synthesized by the end-of-transaction
 * synthesis stage on the router. Returns an empty list if 'shardsPipeline' has no such stages.
 */
DocumentSourceContainer getEndOfTransactionUserStages(
    const Pipeline& shardsPipeline, const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * If 'mergePipeline' contains an end-of-transaction synthesis stage, gives it a copy of the
 * user-defined single-document transformation stages that execute on the shards (taken from
 * 'shardsPipeline'). Called after the router pipeline has been split.
 */
void setEndOfTransactionUserPipeline(const Pipeline& shardsPipeline, Pipeline& mergePipeline);

}  // namespace mongo::change_stream::pipeline_helpers
