/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once


#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/projection_exec.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"

namespace mongo {

class CollatorInterface;
class ExtensionsCallback;

struct ProjectionStageParams {
    enum ProjectionImplementation {
        // The default case.  Will handle every projection.
        NO_FAST_PATH,

        // The projection is simple inclusion and is totally covered by one index.
        COVERED_ONE_INDEX,

        // The projection is simple inclusion and we expect an object.
        SIMPLE_DOC
    };

    ProjectionStageParams(const ExtensionsCallback& wc) : extensionsCallback(&wc) {}

    ProjectionImplementation projImpl = NO_FAST_PATH;

    // The projection object.  We lack a ProjectionExpression or similar so we use a BSONObj.
    BSONObj projObj;

    // If we have a positional or elemMatch projection we need a MatchExpression to pull out the
    // right data.
    // Not owned here, we do not take ownership.
    const MatchExpression* fullExpression = nullptr;

    // If (COVERED_ONE_INDEX == projObj) this is the key pattern we're extracting covered data
    // from.  Otherwise, this field is ignored.
    BSONObj coveredKeyObj;

    // Used for creating context for the match extensions processing. Not owned.
    const ExtensionsCallback* extensionsCallback;

    // The collator this operation should use to compare strings. If null, the collation is a simple
    // binary compare.
    const CollatorInterface* collator = nullptr;
};

/**
 * This stage computes a projection.
 */
class ProjectionStage final : public PlanStage {
public:
    ProjectionStage(OperationContext* opCtx,
                    const ProjectionStageParams& params,
                    WorkingSet* ws,
                    PlanStage* child);

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_PROJECTION;
    }

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const final;

    using FieldSet = StringMap<bool>;  // Value is unused.

    /**
     * Given the projection spec for a simple inclusion projection,
     * 'projObj', populates 'includedFields' with the set of field
     * names to be included.
     */
    static void getSimpleInclusionFields(const BSONObj& projObj, FieldSet* includedFields);

    /**
     * Applies a simple inclusion projection to 'in', including
     * only the fields specified by 'includedFields'.
     *
     * The resulting document is constructed using 'bob'.
     */
    static void transformSimpleInclusion(const BSONObj& in,
                                         const FieldSet& includedFields,
                                         BSONObjBuilder& bob);

    static const char* kStageType;

private:
    Status transform(WorkingSetMember* member);

    std::unique_ptr<ProjectionExec> _exec;

    // _ws is not owned by us.
    WorkingSet* _ws;

    // Stats
    ProjectionStats _specificStats;

    // Fast paths:
    ProjectionStageParams::ProjectionImplementation _projImpl;

    // Used by all projection implementations.
    BSONObj _projObj;

    // Data used for both SIMPLE_DOC and COVERED_ONE_INDEX paths.
    // Has the field names present in the simple projection.
    FieldSet _includedFields;

    //
    // Used for the COVERED_ONE_INDEX path.
    //
    BSONObj _coveredKeyObj;

    // Field names can be empty in 2.4 and before so we can't use them as a sentinel value.
    // If the i-th entry is true we include the i-th field in the key.
    std::vector<bool> _includeKey;

    // If the i-th entry of _includeKey is true this is the field name for the i-th key field.
    std::vector<StringData> _keyFieldNames;
};

}  // namespace mongo
