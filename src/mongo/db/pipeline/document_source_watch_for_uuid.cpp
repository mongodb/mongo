/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_watch_for_uuid.h"

#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/resume_token.h"

namespace mongo {

DocumentSource::GetNextResult DocumentSourceWatchForUUID::getNext() {
    pExpCtx->checkForInterrupt();

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced())
        return nextInput;

    // This single-collection stream was opened before the collection was created, and the pipeline
    // does not know its UUID. When we see the first event, we update our expression context with
    // the UUID drawn from that event's resume token.
    if (!pExpCtx->uuid) {
        auto resumeToken = ResumeToken::parse(
            nextInput.getDocument()[DocumentSourceChangeStream::kIdField].getDocument());
        pExpCtx->uuid = resumeToken.getData().uuid;
    }

    // Forward the result without modification.
    return nextInput;
}

}  // namespace mongo
