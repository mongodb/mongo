/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>
#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"

namespace mongo {

using std::unique_ptr;
using std::vector;

using fts::FTSSpec;

class OperationContext;

/**
 * A blocking stage that returns the set of WSMs with RecordIDs of all of the documents that contain
 * the positive terms in the search query, as well as their scores.
 *
 * The WorkingSetMembers returned are fetched and in the LOC_AND_OBJ state.
 */
class TextOrStage final : public PlanStage {
public:
    /**
     * Internal states.
     */
    enum class State {
        // 1. Initialize the _recordCursor.
        kInit,

        // 2. Read the terms/scores from the text index.
        kReadingTerms,

        // 3. Return results to our parent.
        kReturningResults,

        // 4. Finished.
        kDone,
    };

    TextOrStage(OperationContext* txn,
                const FTSSpec& ftsSpec,
                WorkingSet* ws,
                const MatchExpression* filter,
                IndexDescriptor* index);
    ~TextOrStage();

    void addChild(unique_ptr<PlanStage> child);

    bool isEOF() final;

    StageState doWork(WorkingSetID* out) final;

    void doSaveState() final;
    void doRestoreState() final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;
    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) final;

    StageType stageType() const final {
        return STAGE_TEXT_OR;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    /**
     * Worker for kInit. Initializes the _recordCursor member and handles the potential for
     * getCursor() to throw WriteConflictException.
     */
    StageState initStage(WorkingSetID* out);

    /**
     * Worker for kReadingTerms. Reads from the children, searching for the terms in the query and
     * populates the score map.
     */
    StageState readFromChildren(WorkingSetID* out);

    /**
     * Helper called from readFromChildren to update aggregate score with a newfound (term, score)
     * pair for this document.
     */
    StageState addTerm(WorkingSetID wsid, WorkingSetID* out);

    /**
     * Worker for kReturningResults. Returns a wsm with RecordID and Score.
     */
    StageState returnResults(WorkingSetID* out);

    // The index spec used to determine where to find the score.
    FTSSpec _ftsSpec;

    // Not owned by us.
    WorkingSet* _ws;

    // What state are we in?  See the State enum above.
    State _internalState = State::kInit;

    // Which of _children are we calling work(...) on now?
    size_t _currentChild = 0;

    /**
     *  Temporary score data filled out by children.
     *  Maps from RecordID -> (aggregate score for doc, wsid).
     *  Map each buffered record id to this data.
     */
    struct TextRecordData {
        TextRecordData() : wsid(WorkingSet::INVALID_ID), score(0.0) {}
        WorkingSetID wsid;
        double score;
    };

    typedef unordered_map<RecordId, TextRecordData, RecordId::Hasher> ScoreMap;
    ScoreMap _scores;
    ScoreMap::const_iterator _scoreIterator;

    TextOrStats _specificStats;

    // Members needed only for using the TextMatchableDocument.
    const MatchExpression* _filter;
    WorkingSetID _idRetrying;
    std::unique_ptr<SeekableRecordCursor> _recordCursor;
    IndexDescriptor* _index;
};
}
