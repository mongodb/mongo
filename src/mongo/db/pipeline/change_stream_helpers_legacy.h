/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"

namespace mongo::change_stream_legacy {

/**
 * Looks up and returns a pre-image document at the specified opTime in the oplog. Asserts that if
 * an oplog entry with the given opTime is found, it is a no-op entry with a valid non-empty
 * pre-image document.
 */
boost::optional<Document> legacyLookupPreImage(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                               const Document& preImageId);

/**
 * Represents the change stream operation types that are NOT guarded behind the 'showExpandedEvents'
 * flag.
 */
static const std::set<StringData> kClassicOperationTypes =
    std::set<StringData>{DocumentSourceChangeStream::kUpdateOpType,
                         DocumentSourceChangeStream::kDeleteOpType,
                         DocumentSourceChangeStream::kReplaceOpType,
                         DocumentSourceChangeStream::kInsertOpType,
                         DocumentSourceChangeStream::kDropCollectionOpType,
                         DocumentSourceChangeStream::kRenameCollectionOpType,
                         DocumentSourceChangeStream::kDropDatabaseOpType,
                         DocumentSourceChangeStream::kInvalidateOpType,
                         DocumentSourceChangeStream::kReshardBeginOpType,
                         DocumentSourceChangeStream::kReshardDoneCatchUpOpType,
                         DocumentSourceChangeStream::kNewShardDetectedOpType};

/**
 * Adds filtering for legacy-format {op: 'n'} oplog messages, which used the "o2.type" field to
 * indicate the message type.
 */
void populateInternalOperationFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     BSONArrayBuilder* filter);

/**
 * Converts legacy-format oplog o2 fields of type {type: <op name>, ...} to
 * {..., <op name>: <namespace>}. Does nothing if the 'type' field is not present inside 'o2'.
 */
Document convertFromLegacyOplogFormat(const Document& legacyO2Entry, const NamespaceString& nss);

StringData getNewShardDetectedOpName(const boost::intrusive_ptr<ExpressionContext>& expCtx);

}  // namespace mongo::change_stream_legacy
