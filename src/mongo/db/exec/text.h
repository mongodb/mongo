/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/unordered_map.h"

#include <map>
#include <queue>
#include <vector>

namespace mongo {

    using fts::FTSIndexFormat;
    using fts::FTSMatcher;
    using fts::FTSQuery;
    using fts::FTSSpec;
    using fts::MAX_WEIGHT;

    class OperationContext;

    struct TextStageParams {
        TextStageParams(const FTSSpec& s) : spec(s) {}

        // Text index descriptor.  IndexCatalog owns this.
        IndexDescriptor* index;

        // Index spec.
        FTSSpec spec;

        // Index keys that precede the "text" index key.
        BSONObj indexPrefix;

        // The text query.
        FTSQuery query;
    };

    /**
     * Implements a blocking stage that returns text search results.
     *
     * Prerequisites: None; is a leaf node.
     * Output type: LOC_AND_OBJ_UNOWNED.
     *
     * TODO: Should the TextStage ever generate NEED_FETCH requests? Right now this stage could
     * reduce concurrency by failing to request a yield during fetch.
     */
    class TextStage : public PlanStage {
    public:
        /**
         * The text stage has a few 'states' it transitions between.
         */
        enum State {
            // 1. Initialize the index scans we use to retrieve term/score info.
            INIT_SCANS,

            // 2. Read the terms/scores from the text index.
            READING_TERMS,

            // 3. Return results to our parent.
            RETURNING_RESULTS,

            // 4. Done.
            DONE,
        };

        TextStage(OperationContext* txn,
                  const TextStageParams& params,
                  WorkingSet* ws,
                  const MatchExpression* filter);

        virtual ~TextStage();

        virtual StageState work(WorkingSetID* out);
        virtual bool isEOF();

        virtual void saveState();
        virtual void restoreState(OperationContext* opCtx);
        virtual void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_TEXT; }

        PlanStageStats* getStats();

        virtual const CommonStats* getCommonStats();

        virtual const SpecificStats* getSpecificStats();

        static const char* kStageType;

    private:
        /**
         * Initializes sub-scanners.
         */
        StageState initScans(WorkingSetID* out);

        /**
         * Helper for buffering results array.  Returns NEED_TIME (if any results were produced),
         * IS_EOF, or FAILURE.
         */
        StageState readFromSubScanners(WorkingSetID* out);

        /**
         * Helper called from readFromSubScanners to update aggregate score with a new-found (term,
         * score) pair for this document.  Also rejects documents that don't match this stage's
         * filter.
         */
        void addTerm(const BSONObj key, WorkingSetID wsid);

        /**
         * Possibly return a result.  FYI, this may perform a fetch directly if it is needed to
         * evaluate all filters.
         */
        StageState returnResults(WorkingSetID* out);

        // transactional context for read locks. Not owned by us
        OperationContext* _txn;

        // Parameters of this text stage.
        TextStageParams _params;

        // Text-specific phrase and negated term matcher.
        FTSMatcher _ftsMatcher;

        // Working set. Not owned by us.
        WorkingSet* _ws;

        // Filter. Not owned by us.
        const MatchExpression* _filter;

        // Stats.
        CommonStats _commonStats;
        TextStats _specificStats;

        // What state are we in?  See the State enum above.
        State _internalState;

        // Used in INIT_SCANS and READING_TERMS.  The index scans we're using to retrieve text
        // terms.
        OwnedPointerVector<PlanStage> _scanners;

        // Which _scanners are we currently reading from?
        size_t _currentIndexScanner;

        // Map each buffered record id to this data.
        struct TextRecordData {
            TextRecordData() : wsid(WorkingSet::INVALID_ID), score(0.0) { }
            WorkingSetID wsid;
            double score;
        };

        // Temporary score data filled out by sub-scans.  Used in READING_TERMS and
        // RETURNING_RESULTS.
        // Maps from diskloc -> (aggregate score for doc, wsid).
        typedef unordered_map<RecordId, TextRecordData, RecordId::Hasher> ScoreMap;
        ScoreMap _scores;
        ScoreMap::const_iterator _scoreIterator;
    };

} // namespace mongo
