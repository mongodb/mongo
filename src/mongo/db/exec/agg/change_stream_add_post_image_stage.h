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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream add post image aggregation stage and
 * is part of the execution pipeline. It computes the post-image from the pre-image plus the oplog
 * update modification for the 'fullDocument: "required"' / '"whenAvailable"' modes. The
 * 'fullDocument: "updateLookup"' path lives in ChangeStreamUpdateLookupStage. Its construction is
 * based on DocumentSourceChangeStreamAddPostImage, which handles the optimization part.
 */
class ChangeStreamAddPostImageStage final : public Stage {
public:
    ChangeStreamAddPostImageStage(std::string_view stageName,
                                  const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  const FullDocumentModeEnum& fullDocumentMode);

private:
    /**
     * Performs the lookup to retrieve the full document.
     */
    GetNextResult doGetNext() final;

    /**
     * Computes a post-image by taking a pre-image and applying an update modification that is
     * stored in the oplog entry. Returns boost::none if no pre-image information is available.
     */
    boost::optional<Document> generatePostImage(const Document& updateOp) const;

    /**
     * Determines whether post-images are strictly required or may be included only when available,
     * and whether to return a point-in-time post-image or the most current majority-committed
     * version of the updated document.
     */
    FullDocumentModeEnum _fullDocumentMode = FullDocumentModeEnum::kDefault;
};
}  // namespace mongo::exec::agg
