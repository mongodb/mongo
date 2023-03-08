/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_shardserver_info.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalShardServerInfo,
                         DocumentSourceInternalShardServerInfo::LiteParsed::parse,
                         DocumentSourceInternalShardServerInfo::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalShardServerInfo::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$_internalShardServerInfo must take an empty object but found: "
                          << elem,
            elem.type() == BSONType::Object && elem.Obj().isEmpty());

    return new DocumentSourceInternalShardServerInfo(expCtx);
}

DocumentSource::GetNextResult DocumentSourceInternalShardServerInfo::doGetNext() {
    if (!_didEmit) {
        auto shardName = pExpCtx->mongoProcessInterface->getShardName(pExpCtx->opCtx);
        auto hostAndPort = pExpCtx->mongoProcessInterface->getHostAndPort(pExpCtx->opCtx);
        _didEmit = true;
        return DocumentSource::GetNextResult(DOC("shard" << shardName << "host" << hostAndPort));
    }

    return DocumentSource::GetNextResult::makeEOF();
}

Value DocumentSourceInternalShardServerInfo::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {

    return Value(Document{{getSourceName(), Value{Document{{}}}}});
}

}  // namespace mongo
