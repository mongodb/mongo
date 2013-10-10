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

#include "mongo/db/diskloc.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_search.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
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

    struct TextStageParams {
        TextStageParams(const FTSSpec& s) : spec(s) {}

        // Namespace.
        string ns;

        // Text index descriptor.  TextStage owns this.
        IndexDescriptor* index;

        // Index spec.
        FTSSpec spec;

        // Index keys that precede the "text" index key.
        BSONObj indexPrefix;

        // The text query.
        FTSQuery query;

        // Limit for this stage.
        size_t limit;
    };

    /**
     * Implements a blocking stage that returns text search results.
     *
     * Prerequisites: None; is a leaf node.
     * Output type: LOC_AND_OBJ_UNOWNED.
     */
    class TextStage : public PlanStage {
    public:
        TextStage(const TextStageParams& params, WorkingSet* ws, const MatchExpression* filter);
        virtual ~TextStage();

        virtual StageState work(WorkingSetID* out);
        virtual bool isEOF();

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        PlanStageStats* getStats();

    private:
        // A helper class used for sorting results by score.
        struct ScoredLocation {
            DiskLoc loc;
            double score;

            ScoredLocation() : loc(DiskLoc()), score(1.0) {}
            ScoredLocation(const DiskLoc& d, double s) : loc(d), score(s) {}
            ~ScoredLocation() {}

            // Use descending order (highest-scored documents should appear first).
            bool operator<(const ScoredLocation& rhs) const {
                if (score != rhs.score) {
                    return score > rhs.score;
                }
                return loc < rhs.loc;
            }
        };

        // Helper for buffering results array.  Returns NEED_TIME on success, FAILURE on failure.
        StageState fillOutResults();

        // Helper to update _scores with a new-found (term, score) pair for this document.  Also
        // rejects documents that don't match this stage's filter.
        void filterAndScore(BSONObj key, DiskLoc loc);

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

        // State bit for work().  True if results have been buffered.
        bool _filledOutResults;

        // Map: diskloc -> aggregate score for doc.
        typedef unordered_map<DiskLoc, double, DiskLoc::Hasher> ScoreMap;
        ScoreMap _scores;

        // Score-ordered result set of documents (as DiskLoc's).
        std::vector<ScoredLocation> _results;

        // The next result to return from work().
        size_t _curResult;
    };

} // namespace mongo
