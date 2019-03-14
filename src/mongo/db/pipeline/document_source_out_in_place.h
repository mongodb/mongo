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

#pragma once

#include "mongo/db/pipeline/document_source_out.h"

namespace mongo {

/**
 * Version of $out which performs inserts directly to the output collection, failing if there's a
 * duplicate key.
 */
class DocumentSourceOutInPlace : public DocumentSourceOut {
public:
    using DocumentSourceOut::DocumentSourceOut;

    const NamespaceString& getWriteNs() const final {
        return _outputNs;
    };

    /**
     * No initialization needed since writes will be directed straight to the output collection.
     */
    void initializeWriteNs() final{};

    void finalize() final{};
};


/**
 * Version of $out which replaces the documents in the output collection that match the unique key,
 * or inserts the document if there is no match.
 */
class DocumentSourceOutInPlaceReplace final : public DocumentSourceOutInPlace {
public:
    using DocumentSourceOutInPlace::DocumentSourceOutInPlace;

    void spill(BatchedObjects&& batch) final {
        // Set upsert to true and multi to false as there should be at most one document to update
        // or insert.
        constexpr auto upsert = true;
        constexpr auto multi = false;
        try {
            OutStageWriteBlock writeBlock(pExpCtx->opCtx);

            pExpCtx->mongoProcessInterface->update(pExpCtx,
                                                   getWriteNs(),
                                                   std::move(batch.uniqueKeys),
                                                   std::move(batch.objects),
                                                   _writeConcern,
                                                   upsert,
                                                   multi,
                                                   _targetEpoch());
        } catch (const ExceptionFor<ErrorCodes::ImmutableField>& ex) {
            uassertStatusOKWithContext(ex.toStatus(),
                                       "$out failed to update the matching document, did you "
                                       "attempt to modify the _id or the shard key?");
        }
    }
};

}  // namespace mongo
