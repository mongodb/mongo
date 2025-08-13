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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_index_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/record_store.h"

#include <memory>

namespace mongo {

class IndexAccessMethod;
class RecordCursor;

/**
 * A standalone stage implementing the fast path for key-value retrievals via the _id index. Since
 * the _id index always has the collection default collation, the IDHackStage can only be used when
 * the query's collation is equal to the collection default.
 */
class IDHackStage final : public RequiresIndexStage {
public:
    /** Takes ownership of all the arguments -collection. */
    IDHackStage(ExpressionContext* expCtx,
                CanonicalQuery* query,
                WorkingSet* ws,
                VariantCollectionPtrOrAcquisition collection,
                const IndexDescriptor* descriptor);

    IDHackStage(ExpressionContext* expCtx,
                const BSONObj& key,
                WorkingSet* ws,
                VariantCollectionPtrOrAcquisition collection,
                const IndexDescriptor* descriptor);

    ~IDHackStage() override;

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_IDHACK;
    }

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

protected:
    void doSaveStateRequiresIndex() final;

    void doRestoreStateRequiresIndex() final;

private:
    /**
     * Marks this stage as done, optionally adds key metadata, and returns PlanStage::ADVANCED.
     *
     * Called whenever we have a WSM containing the matching obj.
     */
    StageState advance(WorkingSetID id, WorkingSetMember* member, WorkingSetID* out);

    std::unique_ptr<SeekableRecordCursor> _recordCursor;

    // The WorkingSet we annotate with results.  Not owned by us.
    WorkingSet* _workingSet;

    // The value to match against the _id field.
    BSONObj _key;

    // Have we returned our one document?
    bool _done = false;

    // Do we need to add index key metadata for returnKey?
    bool _addKeyMetadata = false;

    IDHackStats _specificStats;
};

}  // namespace mongo
