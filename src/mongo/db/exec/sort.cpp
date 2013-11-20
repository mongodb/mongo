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

#include "mongo/db/exec/sort.h"

#include <algorithm>

#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/query_planner.h"

namespace mongo {

    const size_t kMaxBytes = 32 * 1024 * 1024;

    struct SortStage::WorkingSetComparator {
        explicit WorkingSetComparator(BSONObj p) : pattern(p) { }

        bool operator()(const SortableDataItem& lhs, const SortableDataItem rhs) const {
            int result = lhs.sortKey.woCompare(rhs.sortKey, pattern, false /* ignore field names */);
            if (0 != result) {
                return result < 0;
            }
            return lhs.loc < rhs.loc;
        }

        BSONObj pattern;
    };

    SortStage::SortStage(const SortStageParams& params, WorkingSet* ws, PlanStage* child)
        : _ws(ws),
          _child(child),
          _pattern(params.pattern),
          _sorted(false),
          _resultIterator(_data.end()),
          _hasBounds(false),
          _memUsage(0) {

        // Fill out _bounds and _hasBounds.
        getBoundsForSort(params.query, params.pattern);

        _cmp.reset(new WorkingSetComparator(_pattern));

        // We'll need to treat arrays as if we were to create an index over them. that is,
        // we may need to unnest the first level and consider each array element to decide
        // the sort order.
        std::vector<const char *> fieldNames;
        std::vector<BSONElement> fixed;
        BSONObjIterator it(_pattern);
        while (it.more()) {
            BSONElement patternElt = it.next();
            fieldNames.push_back(patternElt.fieldName());
            fixed.push_back(BSONElement());
        }

        _keyGen.reset(new BtreeKeyGeneratorV1(fieldNames, fixed, false /* not sparse */));

        if (_hasBounds) {
            // See comment on the operator() call about sort semantics and why we need a
            // to use a bounds checker here.
            _boundsChecker.reset(new IndexBoundsChecker(&_bounds, _pattern, 1 /* == order */));
        }
    }

    SortStage::~SortStage() { }

    void SortStage::getBoundsForSort(const BSONObj& queryObj, const BSONObj& sortObj) {
        QueryPlannerParams params;
        params.options = QueryPlannerParams::NO_TABLE_SCAN;

        IndexEntry sortOrder(sortObj, true, false, "doesnt_matter");
        params.indices.push_back(sortOrder);

        CanonicalQuery* rawQueryForSort;
        verify(CanonicalQuery::canonicalize("fake_ns",
                                            queryObj,
                                            &rawQueryForSort).isOK());
        auto_ptr<CanonicalQuery> queryForSort(rawQueryForSort);

        vector<QuerySolution*> solns;
        QueryPlanner::plan(*queryForSort, params, &solns);

        // TODO: are there ever > 1 solns?  If so, do we look for a specific soln?
        if (1 == solns.size()) {
            IndexScanNode* ixScan = NULL;
            QuerySolutionNode* rootNode = solns[0]->root.get();

            if (rootNode->getType() == STAGE_FETCH) {
                FetchNode* fetchNode = static_cast<FetchNode*>(rootNode);
                if (fetchNode->children[0]->getType() != STAGE_IXSCAN) {
                    delete solns[0];
                    // No bounds.
                    return;
                }
                ixScan = static_cast<IndexScanNode*>(fetchNode->children[0]);
            }
            else if (rootNode->getType() == STAGE_IXSCAN) {
                ixScan = static_cast<IndexScanNode*>(rootNode);
            }

            if (ixScan) {
                // XXX use .swap?
                _bounds = ixScan->bounds;
                _hasBounds = true;
            }
        }

        for (size_t i = 0; i < solns.size(); ++i) {
            delete solns[i];
        }
    }

    bool SortStage::isEOF() {
        // We're done when our child has no more results, we've sorted the child's results, and
        // we've returned all sorted results.
        return _child->isEOF() && _sorted && (_data.end() == _resultIterator);
    }

    PlanStage::StageState SortStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (_memUsage > kMaxBytes) {
            return PlanStage::FAILURE;
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        // Still reading in results to sort.
        if (!_sorted) {
            WorkingSetID id;
            StageState code = _child->work(&id);

            if (PlanStage::ADVANCED == code) {
                // Add it into the map for quick invalidation if it has a valid DiskLoc.
                // A DiskLoc may be invalidated at any time (during a yield).  We need to get into
                // the WorkingSet as quickly as possible to handle it.
                WorkingSetMember* member = _ws->get(id);
                if (member->hasLoc()) {
                    _wsidByDiskLoc[member->loc] = id;
                }

                // Do some accounting to make sure we're not using too much memory.
                if (member->hasLoc()) {
                    _memUsage += sizeof(DiskLoc);
                }

                // We are not supposed (yet) to sort over anything other than objects.  In other
                // words, the query planner wouldn't put a sort atop anything that wouldn't have a
                // collection scan as a leaf.
                verify(member->hasObj());
                _memUsage += member->obj.objsize();

                // We will sort '_data' in the same order an index over '_pattern' would
                // have. This has very nuanced implications. Consider the sort pattern {a:1}
                // and the document {a:[1,10]}. We have potentially two keys we could use to
                // sort on. Here we extract these keys. In the next step we decide which one to
                // use.
                BSONObjCmp patternCmp(_pattern);
                BSONObjSet keys(patternCmp);
                // XXX keyGen will throw on a "parallel array"
                _keyGen->getKeys(member->obj, &keys);

                // To decide which key to use in sorting, we consider not only the sort pattern
                // but also if a given key, matches the query. Assume a query {a: {$gte: 5}} and
                // a document {a:1}. That document wouldn't match. In the same sense, the key '1'
                // in an array {a: [1,10]} should not be considered as being part of the result
                // set and thus that array should sort based on the '10' key. To find such key,
                // we use the bounds for the query.
                BSONObj sortKey;
                if (!_hasBounds) {
                    sortKey = *keys.begin();
                }
                else {
                    verify(NULL != _boundsChecker.get());
                    for (BSONObjSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
                        if (_boundsChecker->isValidKey(*it)) {
                            sortKey = *it;
                            break;
                        }
                    }
                }

                if (sortKey.isEmpty()) {
                    // We assume that if the document made it throught the sort stage, than it
                    // matches the query and thus should contain at least on array item that
                    // is within the query bounds.
                    log() << "can't find bounds for obj " << member->obj.toString() << endl;
                    log() << "bounds are " << _bounds.toString() << endl;
                    verify(0);
                }

                // We let the data stay in the WorkingSet and sort using the selected portion
                // of the object in that working set member.
                SortableDataItem item;
                item.wsid = id;
                item.sortKey = sortKey;
                if (member->hasLoc()) {
                    item.loc = member->loc;
                }
                _data.push_back(item);

                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else if (PlanStage::IS_EOF == code) {
                // TODO: We don't need the lock for this.  We could ask for a yield and do this work
                // unlocked.  Also, this is performing a lot of work for one call to work(...)
                std::sort(_data.begin(), _data.end(), *_cmp);
                _resultIterator = _data.begin();
                _sorted = true;
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else {
                if (PlanStage::NEED_FETCH == code) {
                    *out = id;
                    ++_commonStats.needFetch;
                }
                else if (PlanStage::NEED_TIME == code) {
                    ++_commonStats.needTime;
                }
                return code;
            }
        }

        // Returning results.
        verify(_resultIterator != _data.end());
        verify(_sorted);
        *out = _resultIterator->wsid;
        _resultIterator++;

        // If we're returning something, take it out of our DL -> WSID map so that future
        // calls to invalidate don't cause us to take action for a DL we're done with.
        WorkingSetMember* member = _ws->get(*out);
        if (member->hasLoc()) {
            _wsidByDiskLoc.erase(member->loc);
        }

        // If it was flagged, we just drop it on the floor, assuming the caller wants a DiskLoc.  We
        // could make this triggerable somehow.
        if (_ws->isFlagged(*out)) {
            _ws->free(*out);
            return PlanStage::NEED_TIME;
        }

        ++_commonStats.advanced;
        return PlanStage::ADVANCED;
    }

    void SortStage::prepareToYield() {
        ++_commonStats.yields;
        _child->prepareToYield();
    }

    void SortStage::recoverFromYield() {
        ++_commonStats.unyields;
        _child->recoverFromYield();
    }

    void SortStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;
        _child->invalidate(dl);

        // _data contains indices into the WorkingSet, not actual data.  If a WorkingSetMember in
        // the WorkingSet needs to change state as a result of a DiskLoc invalidation, it will still
        // be at the same spot in the WorkingSet.  As such, we don't need to modify _data.
        DataMap::iterator it = _wsidByDiskLoc.find(dl);

        // If we're holding on to data that's got the DiskLoc we're invalidating...
        if (_wsidByDiskLoc.end() != it) {
            // Grab the WSM that we're nuking.
            WorkingSetMember* member = _ws->get(it->second);
            verify(member->loc == dl);

            // Fetch, invalidate, and flag.
            WorkingSetCommon::fetchAndInvalidateLoc(member);
            _ws->flagForReview(it->second);

            // Remove the DiskLoc from our set of active DLs.
            _wsidByDiskLoc.erase(it);
            ++_specificStats.forcedFetches;
        }
    }

    PlanStageStats* SortStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_SORT));
        ret->specific.reset(new SortStats(_specificStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

}  // namespace mongo
