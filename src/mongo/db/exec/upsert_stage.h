/**
 *    Copyright (C) 2019 MongoDB, Inc.
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

#include "mongo/db/exec/update_stage.h"

namespace mongo {

/**
 * Execution stage for update requests with {upsert:true}. This is a specialized UpdateStage which,
 * in the event that no documents match the update request's query, generates and inserts a new
 * document into the collection. All logic related to the insertion phase is implemented by this
 * class.
 *
 * If the prior or newly-updated version of the document was requested to be returned, then ADVANCED
 * is returned after updating or inserting a document. Otherwise, NEED_TIME is returned after
 * updating a document if further updates are pending, and IS_EOF is returned if all updates have
 * been performed or if a document has been inserted.
 *
 * Callers of doWork() must be holding a write lock.
 */
class UpsertStage final : public UpdateStage {
    UpsertStage(const UpsertStage&) = delete;
    UpsertStage& operator=(const UpsertStage&) = delete;

public:
    UpsertStage(ExpressionContext* expCtx,
                const UpdateStageParams& params,
                WorkingSet* ws,
                Collection* collection,
                PlanStage* child);

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

private:
    BSONObj _produceNewDocumentForInsert();
    void _performInsert(BSONObj newDocument);

    void _generateNewDocumentFromSuppliedDoc(const FieldRefSet& immutablePaths);
    void _generateNewDocumentFromUpdateOp(const FieldRefSet& immutablePaths);

    void _assertDocumentToBeInsertedIsValid(const mutablebson::Document& document,
                                            const FieldRefSet& shardKeyPaths,
                                            bool isInternalRequest,
                                            bool enforceOkForStorage);
};

}  // namespace mongo
