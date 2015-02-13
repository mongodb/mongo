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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/index_names.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"

namespace mongo {

    using std::auto_ptr;
    using std::endl;
    using std::vector;

    // static
    const char* SortStage::kStageType = "SORT";

    SortStageKeyGenerator::SortStageKeyGenerator(const Collection* collection,
                                                 const BSONObj& sortSpec,
                                                 const BSONObj& queryObj) {
        _collection = collection;
        _hasBounds = false;
        _sortHasMeta = false;
        _rawSortSpec = sortSpec;

        // 'sortSpec' can be a mix of $meta and index key expressions.  We pick it apart so that
        // we only generate Btree keys for the index key expressions.

        // The Btree key fields go in here.  We pass this fake index key pattern to the Btree
        // key generator below as part of generating sort keys for the docs.
        BSONObjBuilder btreeBob;

        // The pattern we use to woCompare keys.  Each field in 'sortSpec' will go in here with
        // a value of 1 or -1.  The Btree key fields are verbatim, meta fields have a default.
        BSONObjBuilder comparatorBob;

        BSONObjIterator it(sortSpec);
        while (it.more()) {
            BSONElement elt = it.next();
            if (elt.isNumber()) {
                // Btree key.  elt (should be) foo: 1 or foo: -1.
                comparatorBob.append(elt);
                btreeBob.append(elt);
            }
            else if (LiteParsedQuery::isTextScoreMeta(elt)) {
                // Sort text score decreasing by default.  Field name doesn't matter but we choose
                // something that a user shouldn't ever have.
                comparatorBob.append("$metaTextScore", -1);
                _sortHasMeta = true;
            }
            else {
                // Sort spec. should have been validated before here.
                verify(false);
            }
        }

        // Our pattern for woComparing keys.
        _comparatorObj = comparatorBob.obj();

        // The fake index key pattern used to generate Btree keys.
        _btreeObj = btreeBob.obj();

        // If we're just sorting by meta, don't bother with all the key stuff.
        if (_btreeObj.isEmpty()) {
            return;
        }

        // We'll need to treat arrays as if we were to create an index over them. that is,
        // we may need to unnest the first level and consider each array element to decide
        // the sort order.
        std::vector<const char *> fieldNames;
        std::vector<BSONElement> fixed;
        BSONObjIterator btreeIt(_btreeObj);
        while (btreeIt.more()) {
            BSONElement patternElt = btreeIt.next();
            fieldNames.push_back(patternElt.fieldName());
            fixed.push_back(BSONElement());
        }

        _keyGen.reset(new BtreeKeyGeneratorV1(fieldNames, fixed, false /* not sparse */));

        // The bounds checker only works on the Btree part of the sort key.
        getBoundsForSort(queryObj, _btreeObj);

        if (_hasBounds) {
            _boundsChecker.reset(new IndexBoundsChecker(&_bounds, _btreeObj, 1 /* == order */));
        }
    }

    Status SortStageKeyGenerator::getSortKey(const WorkingSetMember& member,
                                             BSONObj* objOut) const {
        BSONObj btreeKeyToUse;

        Status btreeStatus = getBtreeKey(member.obj.value(), &btreeKeyToUse);
        if (!btreeStatus.isOK()) {
            return btreeStatus;
        }

        if (!_sortHasMeta) {
            *objOut = btreeKeyToUse;
            return Status::OK();
        }

        BSONObjBuilder mergedKeyBob;

        // Merge metadata into the key.
        BSONObjIterator it(_rawSortSpec);
        BSONObjIterator btreeIt(btreeKeyToUse);
        while (it.more()) {
            BSONElement elt = it.next();
            if (elt.isNumber()) {
                // Merge btree key elt.
                mergedKeyBob.append(btreeIt.next());
            }
            else if (LiteParsedQuery::isTextScoreMeta(elt)) {
                // Add text score metadata
                double score = 0.0;
                if (member.hasComputed(WSM_COMPUTED_TEXT_SCORE)) {
                    const TextScoreComputedData* scoreData
                        = static_cast<const TextScoreComputedData*>(
                                member.getComputed(WSM_COMPUTED_TEXT_SCORE));
                    score = scoreData->getScore();
                }
                mergedKeyBob.append("$metaTextScore", score);
            }
        }

        *objOut = mergedKeyBob.obj();
        return Status::OK();
    }

    Status SortStageKeyGenerator::getBtreeKey(const BSONObj& memberObj, BSONObj* objOut) const {
        // Not sorting by anything in the key, just bail out early.
        if (_btreeObj.isEmpty()) {
            *objOut = BSONObj();
            return Status::OK();
        }

        // We will sort '_data' in the same order an index over '_pattern' would have.  This is
        // tricky.  Consider the sort pattern {a:1} and the document {a:[1, 10]}. We have
        // potentially two keys we could use to sort on. Here we extract these keys.
        BSONObjCmp patternCmp(_btreeObj);
        BSONObjSet keys(patternCmp);

        try {
            _keyGen->getKeys(memberObj, &keys);
        }
        catch (const UserException& e) {
            // Probably a parallel array.
            if (BtreeKeyGenerator::ParallelArraysCode == e.getCode()) {
                return Status(ErrorCodes::BadValue,
                              "cannot sort with keys that are parallel arrays");
            }
            else {
                return e.toStatus();
            }
        }
        catch (...) {
            return Status(ErrorCodes::InternalError, "unknown error during sort key generation");
        }

        // Key generator isn't sparse so we should at least get an all-null key.
        invariant(!keys.empty());

        // No bounds?  No problem!  Use the first key.
        if (!_hasBounds) {
            // Note that we sort 'keys' according to the pattern '_btreeObj'.
            *objOut = *keys.begin();
            return Status::OK();
        }

        // To decide which key to use in sorting, we must consider not only the sort pattern but
        // the query.  Assume we have the query {a: {$gte: 5}} and a document {a:1}.  That
        // document wouldn't match the query.  As such, the key '1' in an array {a: [1, 10]}
        // should not be considered as being part of the result set and thus that array cannot
        // sort using the key '1'.  To ensure that the keys we sort by are valid w.r.t. the
        // query we use a bounds checker.
        verify(NULL != _boundsChecker.get());
        for (BSONObjSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
            if (_boundsChecker->isValidKey(*it)) {
                *objOut = *it;
                return Status::OK();
            }
        }

        // No key is in our bounds.
        // TODO: will this ever happen?  don't think it should.
        *objOut = *keys.begin();
        return Status::OK();
    }

    void SortStageKeyGenerator::getBoundsForSort(const BSONObj& queryObj, const BSONObj& sortObj) {
        QueryPlannerParams params;
        params.options = QueryPlannerParams::NO_TABLE_SCAN;

        // We're creating a "virtual index" with key pattern equal to the sort order.
        IndexEntry sortOrder(sortObj, IndexNames::BTREE, true, false, false, "doesnt_matter",
                             BSONObj());
        params.indices.push_back(sortOrder);

        CanonicalQuery* rawQueryForSort;
        verify(CanonicalQuery::canonicalize(
                "fake_ns", queryObj, &rawQueryForSort, WhereCallbackNoop()).isOK());
        auto_ptr<CanonicalQuery> queryForSort(rawQueryForSort);

        vector<QuerySolution*> solns;
        QLOG() << "Sort stage: Planning to obtain bounds for sort." << endl;
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
                _bounds.fields.swap(ixScan->bounds.fields);
                _hasBounds = true;
            }
        }

        for (size_t i = 0; i < solns.size(); ++i) {
            delete solns[i];
        }
    }

    SortStage::WorkingSetComparator::WorkingSetComparator(BSONObj p) : pattern(p) { }

    bool SortStage::WorkingSetComparator::operator()(const SortableDataItem& lhs, const SortableDataItem& rhs) const {
        // False means ignore field names.
        int result = lhs.sortKey.woCompare(rhs.sortKey, pattern, false);
        if (0 != result) {
            return result < 0;
        }
        // Indices use RecordId as an additional sort key so we must as well.
        return lhs.loc < rhs.loc;
    }

    SortStage::SortStage(const SortStageParams& params,
                         WorkingSet* ws,
                         PlanStage* child)
        : _collection(params.collection),
          _ws(ws),
          _child(child),
          _pattern(params.pattern),
          _query(params.query),
          _limit(params.limit),
          _sorted(false),
          _resultIterator(_data.end()),
          _commonStats(kStageType),
          _memUsage(0) {
    }

    SortStage::~SortStage() { }

    bool SortStage::isEOF() {
        // We're done when our child has no more results, we've sorted the child's results, and
        // we've returned all sorted results.
        return _child->isEOF() && _sorted && (_data.end() == _resultIterator);
    }

    PlanStage::StageState SortStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (NULL == _sortKeyGen) {
            // This is heavy and should be done as part of work().
            _sortKeyGen.reset(new SortStageKeyGenerator(_collection, _pattern, _query));
            _sortKeyComparator.reset(new WorkingSetComparator(_sortKeyGen->getSortComparator()));
            // If limit > 1, we need to initialize _dataSet here to maintain ordered
            // set of data items while fetching from the child stage.
            if (_limit > 1) {
                const WorkingSetComparator& cmp = *_sortKeyComparator;
                _dataSet.reset(new SortableDataItemSet(cmp));
            }
            return PlanStage::NEED_TIME;
        }

        const size_t maxBytes = static_cast<size_t>(internalQueryExecMaxBlockingSortBytes);
        if (_memUsage > maxBytes) {
            mongoutils::str::stream ss;
            ss << "sort stage buffered data usage of " << _memUsage
               << " bytes exceeds internal limit of " << maxBytes << " bytes";
            Status status(ErrorCodes::Overflow, ss);
            *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            return PlanStage::FAILURE;
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        // Still reading in results to sort.
        if (!_sorted) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            StageState code = _child->work(&id);

            if (PlanStage::ADVANCED == code) {
                // Add it into the map for quick invalidation if it has a valid RecordId.
                // A RecordId may be invalidated at any time (during a yield).  We need to get into
                // the WorkingSet as quickly as possible to handle it.
                WorkingSetMember* member = _ws->get(id);

                // Planner must put a fetch before we get here.
                verify(member->hasObj());

                // We might be sorting something that was invalidated at some point.
                if (member->hasLoc()) {
                    _wsidByDiskLoc[member->loc] = id;
                }

                // The data remains in the WorkingSet and we wrap the WSID with the sort key.
                SortableDataItem item;
                Status sortKeyStatus = _sortKeyGen->getSortKey(*member, &item.sortKey);
                if (!_sortKeyGen->getSortKey(*member, &item.sortKey).isOK()) {
                    *out = WorkingSetCommon::allocateStatusMember(_ws, sortKeyStatus);
                    return PlanStage::FAILURE;
                }
                item.wsid = id;
                if (member->hasLoc()) {
                    // The RecordId breaks ties when sorting two WSMs with the same sort key.
                    item.loc = member->loc;
                }

                addToBuffer(item);

                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else if (PlanStage::IS_EOF == code) {
                // TODO: We don't need the lock for this.  We could ask for a yield and do this work
                // unlocked.  Also, this is performing a lot of work for one call to work(...)
                sortBuffer();
                _resultIterator = _data.begin();
                _sorted = true;
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else if (PlanStage::FAILURE == code) {
                *out = id;
                // If a stage fails, it may create a status WSM to indicate why it
                // failed, in which case 'id' is valid.  If ID is invalid, we
                // create our own error message.
                if (WorkingSet::INVALID_ID == id) {
                    mongoutils::str::stream ss;
                    ss << "sort stage failed to read in results to sort from child";
                    Status status(ErrorCodes::InternalError, ss);
                    *out = WorkingSetCommon::allocateStatusMember( _ws, status);
                }
                return code;
            }
            else if (PlanStage::NEED_TIME == code) {
                ++_commonStats.needTime;
            }
            else if (PlanStage::NEED_YIELD == code) {
                ++_commonStats.needYield;
                *out = id;
            }

            return code;
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

        ++_commonStats.advanced;
        return PlanStage::ADVANCED;
    }

    void SortStage::saveState() {
        ++_commonStats.yields;
        _child->saveState();
    }

    void SortStage::restoreState(OperationContext* opCtx) {
        ++_commonStats.unyields;
        _child->restoreState(opCtx);
    }

    void SortStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(txn, dl, type);

        // If we have a deletion, we can fetch and carry on.
        // If we have a mutation, it's easier to fetch and use the previous document.
        // So, no matter what, fetch and keep the doc in play.

        // _data contains indices into the WorkingSet, not actual data.  If a WorkingSetMember in
        // the WorkingSet needs to change state as a result of a RecordId invalidation, it will still
        // be at the same spot in the WorkingSet.  As such, we don't need to modify _data.
        DataMap::iterator it = _wsidByDiskLoc.find(dl);

        // If we're holding on to data that's got the RecordId we're invalidating...
        if (_wsidByDiskLoc.end() != it) {
            // Grab the WSM that we're nuking.
            WorkingSetMember* member = _ws->get(it->second);
            verify(member->loc == dl);

            WorkingSetCommon::fetchAndInvalidateLoc(txn, member, _collection);

            // Remove the RecordId from our set of active DLs.
            _wsidByDiskLoc.erase(it);
            ++_specificStats.forcedFetches;
        }
    }

    vector<PlanStage*> SortStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* SortStage::getStats() {
        _commonStats.isEOF = isEOF();
        const size_t maxBytes = static_cast<size_t>(internalQueryExecMaxBlockingSortBytes);
        _specificStats.memLimit = maxBytes;
        _specificStats.memUsage = _memUsage;
        _specificStats.limit = _limit;
        _specificStats.sortPattern = _pattern.getOwned();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_SORT));
        ret->specific.reset(new SortStats(_specificStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* SortStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* SortStage::getSpecificStats() {
        return &_specificStats;
    }

    /**
     * addToBuffer() and sortBuffer() work differently based on the
     * configured limit. addToBuffer() is also responsible for
     * performing some accounting on the overall memory usage to
     * make sure we're not using too much memory.
     *
     * limit == 0:
     *     addToBuffer() - Adds item to vector.
     *     sortBuffer() - Sorts vector.
     * limit == 1:
     *     addToBuffer() - Replaces first item in vector with max of
     *                     current and new item.
     *                     Updates memory usage if item was replaced.
     *     sortBuffer() - Does nothing.
     * limit > 1:
     *     addToBuffer() - Does not update vector. Adds item to set.
     *                     If size of set exceeds limit, remove item from set
     *                     with lowest key. Updates memory usage accordingly.
     *     sortBuffer() - Copies items from set to vectors.
     */
    void SortStage::addToBuffer(const SortableDataItem& item) {
        // Holds ID of working set member to be freed at end of this function.
        WorkingSetID wsidToFree = WorkingSet::INVALID_ID;

        if (_limit == 0) {
            _data.push_back(item);
            _memUsage += _ws->get(item.wsid)->getMemUsage();
        }
        else if (_limit == 1) {
            if (_data.empty()) {
                _data.push_back(item);
                _memUsage = _ws->get(item.wsid)->getMemUsage();
                return;
            }
            wsidToFree = item.wsid;
            const WorkingSetComparator& cmp = *_sortKeyComparator;
            // Compare new item with existing item in vector.
            if (cmp(item, _data[0])) {
                wsidToFree = _data[0].wsid;
                _data[0] = item;
                _memUsage = _ws->get(item.wsid)->getMemUsage();
            }
        }
        else {
            // Update data item set instead of vector
            // Limit not reached - insert and return
            vector<SortableDataItem>::size_type limit(_limit);
            if (_dataSet->size() < limit) {
                _dataSet->insert(item);
                _memUsage += _ws->get(item.wsid)->getMemUsage();
                return;
            }
            // Limit will be exceeded - compare with item with lowest key
            // If new item does not have a lower key value than last item,
            // do nothing.
            wsidToFree = item.wsid;
            SortableDataItemSet::const_iterator lastItemIt = --(_dataSet->end());
            const SortableDataItem& lastItem = *lastItemIt;
            const WorkingSetComparator& cmp = *_sortKeyComparator;
            if (cmp(item, lastItem)) {
                _memUsage -= _ws->get(lastItem.wsid)->getMemUsage();
                _memUsage += _ws->get(item.wsid)->getMemUsage();
                wsidToFree = lastItem.wsid;
                // According to std::set iterator validity rules,
                // it does not matter which of erase()/insert() happens first.
                // Here, we choose to erase first to release potential resources
                // used by the last item and to keep the scope of the iterator to a minimum.
                _dataSet->erase(lastItemIt);
                _dataSet->insert(item);
            }
        }

        // If the working set ID is valid, remove from
        // RecordId invalidation map and free from working set.
        if (wsidToFree != WorkingSet::INVALID_ID) {
            WorkingSetMember* member = _ws->get(wsidToFree);
            if (member->hasLoc()) {
                _wsidByDiskLoc.erase(member->loc);
            }
            _ws->free(wsidToFree);
        }
    }

    void SortStage::sortBuffer() {
        if (_limit == 0) {
            const WorkingSetComparator& cmp = *_sortKeyComparator;
            std::sort(_data.begin(), _data.end(), cmp);
        }
        else if (_limit == 1) {
            // Buffer contains either 0 or 1 item so it is already in a sorted state.
            return;
        }
        else {
            // Set already contains items in sorted order, so we simply copy the items
            // from the set to the vector.
            // Release the memory for the set after the copy.
            vector<SortableDataItem> newData(_dataSet->begin(), _dataSet->end());
            _data.swap(newData);
            _dataSet.reset();
        }
    }

}  // namespace mongo
