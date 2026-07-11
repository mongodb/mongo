// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

using fts::FTSSpec;

class OperationContext;

/**
 * A blocking stage that returns the set of WSMs with RecordIDs of all of the documents that contain
 * the positive terms in the search query, as well as their scores.
 *
 * The WorkingSetMembers returned are fetched and in the LOC_AND_OBJ state.
 */
class TextOrStage final : public RequiresCollectionStage {
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

    TextOrStage(ExpressionContext* expCtx,
                size_t keyPrefixSize,
                WorkingSet* ws,
                const MatchExpression* filter,
                CollectionAcquisition collection);

    void addChild(std::unique_ptr<PlanStage> child);

    void addChildren(Children childrenToAdd);

    bool isEOF() const final;

    StageState doWork(WorkingSetID* out) final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_TEXT_OR;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    const SimpleMemoryUsageTracker& getMemoryTracker_forTest() const {
        return _memoryTracker;
    }

    static constexpr std::string_view kStageType = "TEXT_OR"sv;

    class Comparator {
    public:
        explicit Comparator() {}
        int operator()(const RecordId& lhs, const RecordId& rhs) const {
            return lhs.compare(rhs);
        }
    };

protected:
    void doSaveStateRequiresCollection() final;

    void doRestoreStateRequiresCollection() final;

private:
    static constexpr double kRejectedDocumentScore = -1;
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
    StageState returnResultsInMemory(WorkingSetID* out);
    StageState returnResultsSpilled(WorkingSetID* out);

    void doForceSpill() override;

    void initSorter();

    // The key prefix length within a possibly compound key: {prefix,term,score,suffix}.
    const size_t _keyPrefixSize;

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

    typedef absl::flat_hash_map<RecordId, TextRecordData, RecordId::Hasher> ScoreMap;
    ScoreMap _scores;

    struct TextRecordDataForSorter {
        SortableWorkingSetMember document;
        double score;

        TextRecordDataForSorter getOwned() const {
            return {document.getOwned(), score};
        }

        void makeOwned() {
            document.makeOwned();
        }

        int memUsageForSorter() const {
            return document.memUsageForSorter() + sizeof(double);
        }

        struct SorterDeserializeSettings {};

        void serializeForSorter(BufBuilder& buf) const {
            document.serializeForSorter(buf);
            buf.appendNum(score);
        }

        static TextRecordDataForSorter deserializeForSorter(BufReader& buf,
                                                            const SorterDeserializeSettings&) {
            TextRecordDataForSorter result;
            result.document = SortableWorkingSetMember::deserializeForSorter(buf, {});
            result.score = buf.read<LittleEndian<double>>();
            return result;
        }
    };

    // Only used when spilling.
    std::unique_ptr<SorterFileStats> _sorterStats;
    std::unique_ptr<Sorter<RecordId, TextRecordDataForSorter>> _sorter;
    std::unique_ptr<Sorter<RecordId, TextRecordDataForSorter>::Iterator> _sorterIterator;

    TextOrStats _specificStats;

    SimpleMemoryUsageTracker _memoryTracker;

    // Members needed only for using the TextMatchableDocument.
    const MatchExpression* _filter;
    WorkingSetID _idRetrying;
    std::unique_ptr<SeekableRecordCursor> _recordCursor;
};
}  // namespace mongo
