// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::change_stream::topology_helpers {

/**
 * Given the 'originalAggregateCommand' and a resume token, returns a new BSON object with the same
 * command except with the addition of a resumeAfter option containing the resume token. If there
 * was a previous resumeAfter option, it will be removed.
 * The change stream version can be optionally specified to inject a specific change stream reader
 * version into the pipeline command.
 */
BSONObj replaceResumeTokenAndVersionInCommand(
    Document resumeToken,
    const BSONObj& originalAggregateCommand,
    const boost::optional<ChangeStreamReaderVersionEnum>& changeStreamVersion);

/**
 * Updates the $changeStream stage in the 'originalAggregateCommand' to reflect the start time for
 * the newly-added shard(s), then generates the final command object to be run on those shards.
 * The change stream version can be optionally specified to force a specific change stream reader
 * version on the shard.
 */
BSONObj createUpdatedCommandForNewShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ResumeToken& resumeTokenForNewShard,
    const BSONObj& originalAggregateCommand,
    const boost::optional<ChangeStreamReaderVersionEnum>& changeStreamVersion);

/**
 * Calls createUpdatedCommandForNewShard() with the HighWaterMark ResumeToken created from the
 * 'atClusterTime' timestamp.
 */
BSONObj createUpdatedCommandForNewShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp atClusterTime,
    const BSONObj& originalAggregateCommand,
    const boost::optional<ChangeStreamReaderVersionEnum>& changeStreamVersion);

}  // namespace mongo::change_stream::topology_helpers
