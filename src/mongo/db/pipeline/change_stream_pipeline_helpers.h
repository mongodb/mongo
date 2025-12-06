/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

namespace mongo::change_stream::pipeline_helpers {

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

}  // namespace mongo::change_stream::pipeline_helpers
