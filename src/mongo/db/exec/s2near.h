/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include <queue>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/platform/unordered_set.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    struct S2NearParams {
        S2NearParams() : collection(NULL) { }
        Collection* collection;
        BSONObj indexKeyPattern;
        NearQuery nearQuery;
        IndexBounds baseBounds;
        MatchExpression* filter;
        bool addPointMeta;
        bool addDistMeta;
    };

    /**
     * Executes a geoNear search.  Is a leaf node.  Output type is LOC_AND_UNOWNED_OBJ.
     */
    class S2NearStage : public PlanStage {
    public:
        /**
         * Takes: index to scan over, MatchExpression with near point, other MatchExpressions for
         * covered data,
         */
        S2NearStage(const S2NearParams& params, WorkingSet* ws);

        virtual ~S2NearStage();

        StageState work(WorkingSetID* out);
        bool isEOF();

        void prepareToYield();
        void recoverFromYield();
        void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_GEO_NEAR_2DSPHERE; }

        PlanStageStats* getStats();

        static const char* kStageType;

    private:
        void init();
        StageState addResultToQueue(WorkingSetID* out);
        void nextAnnulus();

        bool _worked;

        S2NearParams _params;

        WorkingSet* _ws;

        // This is the "array index" of the key field that is the near field.  We use this to do
        // cheap is-this-doc-in-the-annulus testing.  We also need to know where to stuff the index
        // bounds for the various annuluses/annuli.
        int _nearFieldIndex;

        // Geo filter in index scan (which is owned by fetch stage in _child).
        scoped_ptr<MatchExpression> _keyGeoFilter;

        scoped_ptr<PlanStage> _child;

        // The S2 machinery that represents the search annulus.  We keep this around after bounds
        // generation to check for intersection.
        S2Cap _innerCap;
        S2Cap _outerCap;
        S2RegionIntersection _annulus;

        // We use this to hold on to the results in an annulus.  Results are sorted to have
        // decreasing distance.
        struct Result {
            Result(WorkingSetID wsid, double dist) : id(wsid), distance(dist) { }

            bool operator<(const Result& other) const {
                // We want increasing distance, not decreasing, so we reverse the <.
                return distance > other.distance;
            }

            WorkingSetID id;
            double distance;
        };

        // Our index scans aren't deduped so we might see the same doc twice in a given
        // annulus.
        unordered_set<DiskLoc, DiskLoc::Hasher> _seenInScan;

        // We compute an annulus of results and cache it here.
        priority_queue<Result> _results;

        // For fast invalidation.  Perhaps not worth it.
        unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher> _invalidationMap;

        // Geo-related variables.
        // At what min distance (arc length) do we start looking for results?
        double _minDistance;
        // What's the max distance (arc length) we're willing to look for results?
        double _maxDistance;

        // These radii define the annulus we're currently looking at.
        double _innerRadius;
        double _outerRadius;

        // True if we are looking at last annulus
        bool _outerRadiusInclusive;

        // When we search the next annulus, what to adjust our radius by?  Grows when we search an
        // annulus and find no results.
        double _radiusIncrement;

        // Did we encounter an unrecoverable error?
        bool _failed;

        // Have we init()'d yet?
        bool _initted;

        // What index are we searching over?
        IndexDescriptor* _descriptor;

        CommonStats _commonStats;
    };

}  // namespace mongo
