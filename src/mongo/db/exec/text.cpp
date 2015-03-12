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

#include "mongo/db/exec/text.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"

namespace mongo {

    using std::auto_ptr;
    using std::string;
    using std::vector;

    // static
    const char* TextStage::kStageType = "TEXT";

    TextStage::TextStage(OperationContext* txn,
                         const TextStageParams& params,
                         WorkingSet* ws,
                         const MatchExpression* filter)
        : _txn(txn),
          _params(params),
          _ftsMatcher(params.query, params.spec),
          _ws(ws),
          _filter(filter),
          _commonStats(kStageType),
          _internalState(INIT_SCANS),
          _currentIndexScanner(0),
          _idRetrying(WorkingSet::INVALID_ID) {
        _scoreIterator = _scores.end();
        _specificStats.indexPrefix = _params.indexPrefix;
        _specificStats.indexName = _params.index->indexName();
    }

    TextStage::~TextStage() { }

    bool TextStage::isEOF() {
        return _internalState == DONE;
    }

    PlanStage::StageState TextStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }
        invariant(_internalState != DONE);

        PlanStage::StageState stageState = PlanStage::IS_EOF;

        switch (_internalState) {
        case INIT_SCANS:
            try {
                stageState = initScans(out);
            }
            catch (const WriteConflictException& wce) {
                // Reset and try again next time.
                _internalState = INIT_SCANS;
                _scanners.clear();
                *out = WorkingSet::INVALID_ID;
                stageState = NEED_YIELD;
            }
            break;

        case READING_TERMS:
            stageState = readFromSubScanners(out);
            break;
        case RETURNING_RESULTS:
            stageState = returnResults(out);
            break;
        case DONE:
            // Handled above.
            break;
        }

        // Increment common stats counters that are specific to the return value of work().
        switch (stageState) {
        case PlanStage::ADVANCED:
            ++_commonStats.advanced;
            break;
        case PlanStage::NEED_TIME:
            ++_commonStats.needTime;
            break;
        case PlanStage::NEED_YIELD:
            ++_commonStats.needYield;
            break;
        default:
            break;
        }

        return stageState;
    }

    void TextStage::saveState() {
        _txn = NULL;
        ++_commonStats.yields;

        for (size_t i = 0; i < _scanners.size(); ++i) {
            _scanners.mutableVector()[i]->saveState();
        }
    }

    void TextStage::restoreState(OperationContext* opCtx) {
        invariant(_txn == NULL);
        _txn = opCtx;
        ++_commonStats.unyields;

        for (size_t i = 0; i < _scanners.size(); ++i) {
            _scanners.mutableVector()[i]->restoreState(opCtx);
        }
    }

    void TextStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        // Propagate invalidate to children.
        for (size_t i = 0; i < _scanners.size(); ++i) {
            _scanners.mutableVector()[i]->invalidate(txn, dl, type);
        }

        // We store the score keyed by RecordId.  We have to toss out our state when the RecordId
        // changes.
        // TODO: If we're RETURNING_RESULTS we could somehow buffer the object.
        ScoreMap::iterator scoreIt = _scores.find(dl);
        if (scoreIt != _scores.end()) {
            if (scoreIt == _scoreIterator) {
                _scoreIterator++;
            }
            _scores.erase(scoreIt);
        }
    }

    vector<PlanStage*> TextStage::getChildren() const {
        vector<PlanStage*> empty;
        return empty;
    }

    PlanStageStats* TextStage::getStats() {
        _commonStats.isEOF = isEOF();

        // Add a BSON representation of the filter to the stats tree, if there is one.
        if (NULL != _filter) {
            BSONObjBuilder bob;
            _filter->toBSON(&bob);
            _commonStats.filter = bob.obj();
        }

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_TEXT));
        ret->specific.reset(new TextStats(_specificStats));
        return ret.release();
    }

    const CommonStats* TextStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* TextStage::getSpecificStats() {
        return &_specificStats;
    }

    PlanStage::StageState TextStage::initScans(WorkingSetID* out) {
        invariant(0 == _scanners.size());

        _specificStats.parsedTextQuery = _params.query.toBSON();

        // Get all the index scans for each term in our query.
        // TODO it would be more efficient to only have one active scan at a time and create the
        // next when each finishes.
        for (std::set<std::string>::const_iterator it = _params.query.getTermsForBounds().begin();
             it != _params.query.getTermsForBounds().end();
             ++it) {
            const string& term = *it;
            IndexScanParams params;
            params.bounds.startKey = FTSIndexFormat::getIndexKey(MAX_WEIGHT,
                                                                 term,
                                                                 _params.indexPrefix,
                                                                 _params.spec.getTextIndexVersion());
            params.bounds.endKey = FTSIndexFormat::getIndexKey(0,
                                                               term,
                                                                _params.indexPrefix,
                                                               _params.spec.getTextIndexVersion());
            params.bounds.endKeyInclusive = true;
            params.bounds.isSimpleRange = true;
            params.descriptor = _params.index;
            params.direction = -1;
            _scanners.mutableVector().push_back(new IndexScan(_txn, params, _ws, NULL));
        }

        // If we have no terms we go right to EOF.
        if (0 == _scanners.size()) {
            _internalState = DONE;
            return PlanStage::IS_EOF;
        }

        // Transition to the next state.
        _internalState = READING_TERMS;
        return PlanStage::NEED_TIME;
    }

    PlanStage::StageState TextStage::readFromSubScanners(WorkingSetID* out) {
        // This should be checked before we get here.
        invariant(_currentIndexScanner < _scanners.size());

        // Either retry the last WSM we worked on or get a new one from our current scanner.
        WorkingSetID id;
        StageState childState;
        if (_idRetrying == WorkingSet::INVALID_ID) {
            childState = _scanners.vector()[_currentIndexScanner]->work(&id);
        }
        else {
            childState = ADVANCED;
            id = _idRetrying;
            _idRetrying = WorkingSet::INVALID_ID;
        }

        if (PlanStage::ADVANCED == childState) {
            return addTerm(id, out);
        }
        else if (PlanStage::IS_EOF == childState) {
            // Done with this scan.
            ++_currentIndexScanner;

            if (_currentIndexScanner < _scanners.size()) {
                // We have another scan to read from.
                return PlanStage::NEED_TIME;
            }

            // If we're here we are done reading results.  Move to the next state.
            _scoreIterator = _scores.begin();
            _internalState = RETURNING_RESULTS;

            // Don't need to keep these around.
            _scanners.clear();
            return PlanStage::NEED_TIME;
        }
        else {
            // Propagate WSID from below.
            *out = id;
            if (PlanStage::FAILURE == childState) {
                // If a stage fails, it may create a status WSM to indicate why it
                // failed, in which case 'id' is valid.  If ID is invalid, we
                // create our own error message.
                if (WorkingSet::INVALID_ID == id) {
                    mongoutils::str::stream ss;
                    ss << "text stage failed to read in results from child";
                    Status status(ErrorCodes::InternalError, ss);
                    *out = WorkingSetCommon::allocateStatusMember( _ws, status);
                }
            }
            return childState;
        }
    }

    PlanStage::StageState TextStage::returnResults(WorkingSetID* out) {
        if (_scoreIterator == _scores.end()) {
            _internalState = DONE;
            return PlanStage::IS_EOF;
        }

        // Filter for phrases and negative terms, score and truncate.
        TextRecordData textRecordData = _scoreIterator->second;

        // Ignore non-matched documents.
        if (textRecordData.score < 0) {
            _scoreIterator++;
            invariant(textRecordData.wsid == WorkingSet::INVALID_ID);
            return PlanStage::NEED_TIME;
        }

        WorkingSetMember* wsm = _ws->get(textRecordData.wsid);
        try {
            if (!WorkingSetCommon::fetchIfUnfetched(_txn, wsm, _params.index->getCollection())) {
                _scoreIterator++;
                _ws->free(textRecordData.wsid);
                _commonStats.needTime++;
                return NEED_TIME;
            }
        }
        catch (const WriteConflictException& wce) {
            // Do this record again next time around.
            *out = WorkingSet::INVALID_ID;
            _commonStats.needYield++;
            return NEED_YIELD;
        }

        _scoreIterator++;

        // Filter for phrases and negated terms
        if (!_ftsMatcher.matches(wsm->obj.value())) {
            _ws->free(textRecordData.wsid);
            return PlanStage::NEED_TIME;
        }

        // Populate the working set member with the text score and return it.
        wsm->addComputed(new TextScoreComputedData(textRecordData.score));
        *out = textRecordData.wsid;
        return PlanStage::ADVANCED;
    }

    class TextMatchableDocument : public MatchableDocument {
    public:
        TextMatchableDocument(OperationContext* txn,
                              const BSONObj& keyPattern,
                              const BSONObj& key,
                              WorkingSetMember* wsm,
                              const Collection* collection)
            : _txn(txn),
              _collection(collection),
              _keyPattern(keyPattern),
              _key(key),
              _wsm(wsm) { }

        BSONObj toBSON() const {
            return getObj();
        }

        virtual ElementIterator* allocateIterator(const ElementPath* path) const {
            if (!_wsm->hasObj()) {
                // Try to look in the key.
                BSONObjIterator keyPatternIt(_keyPattern);
                BSONObjIterator keyDataIt(_key);

                while (keyPatternIt.more()) {
                    BSONElement keyPatternElt = keyPatternIt.next();
                    verify(keyDataIt.more());
                    BSONElement keyDataElt = keyDataIt.next();

                    if (path->fieldRef().equalsDottedField(keyPatternElt.fieldName())) {
                        if (Array == keyDataElt.type()) {
                            return new SimpleArrayElementIterator(keyDataElt, true);
                        }
                        else {
                            return new SingleElementElementIterator(keyDataElt);
                        }
                    }
                }
            }

            // Go to the raw document, fetching if needed.
            return new BSONElementIterator(path, getObj());
        }

        virtual void releaseIterator( ElementIterator* iterator ) const {
            delete iterator;
        }

        // Thrown if we detect that the document being matched was deleted.
        class DocumentDeletedException {};

    private:
        BSONObj getObj() const {
            if (!WorkingSetCommon::fetchIfUnfetched(_txn, _wsm, _collection))
                throw DocumentDeletedException();

            return _wsm->obj.value();
        }

        OperationContext* _txn;
        const Collection* _collection;
        BSONObj _keyPattern;
        BSONObj _key;
        WorkingSetMember* _wsm;
    };

    PlanStage::StageState TextStage::addTerm(WorkingSetID wsid, WorkingSetID* out) {
        WorkingSetMember* wsm = _ws->get(wsid);
        invariant(wsm->state == WorkingSetMember::LOC_AND_IDX);
        invariant(1 == wsm->keyData.size());
        const IndexKeyDatum newKeyData = wsm->keyData.back(); // copy to keep it around.

        TextRecordData* textRecordData = &_scores[wsm->loc];
        double* documentAggregateScore = &textRecordData->score;

        if (WorkingSet::INVALID_ID == textRecordData->wsid) {
            // We haven't seen this RecordId before. Keep the working set member around
            // (it may be force-fetched on saveState()).
            textRecordData->wsid = wsid;

            if (_filter) {
                // We have not seen this document before and need to apply a filter.
                bool shouldKeep;
                bool wasDeleted = false;
                try {
                    TextMatchableDocument tdoc(_txn,
                                               newKeyData.indexKeyPattern,
                                               newKeyData.keyData,
                                               wsm,
                                               _params.index->getCollection());
                    shouldKeep = _filter->matches(&tdoc);
                }
                catch (const WriteConflictException& wce) {
                    _idRetrying = wsid;
                    *out = WorkingSet::INVALID_ID;
                    return NEED_YIELD;
                }
                catch (const TextMatchableDocument::DocumentDeletedException&) {
                    // We attempted to fetch the document but decided it should be excluded from the
                    // result set.
                    shouldKeep = false;
                    wasDeleted = true;
                }

                if (!shouldKeep) {
                    if (wasDeleted || wsm->hasObj()) {
                        // We had to fetch but we're not going to return it.
                        ++_specificStats.fetches;
                    }
                    _ws->free(textRecordData->wsid);
                    textRecordData->wsid = WorkingSet::INVALID_ID;
                    *documentAggregateScore = -1;
                    return NEED_TIME;
                }
            }
            else {
                // If we're here, we're going to return the doc, and we do a fetch later.
                ++_specificStats.fetches;
            }
        }
        else {
            // We already have a working set member for this RecordId. Free the new
            // WSM and retrieve the old one.
            // Note that since we don't keep all index keys, we could get a score that doesn't match
            // the document, but this has always been a problem.
            // TODO something to improve the situation.
            invariant(wsid != textRecordData->wsid);
            _ws->free(wsid);
            wsm = _ws->get(textRecordData->wsid);
        }

        ++_specificStats.keysExamined;

        if (*documentAggregateScore < 0) {
            // We have already rejected this document for not matching the filter.
            return NEED_TIME;
        }

        // Locate score within possibly compound key: {prefix,term,score,suffix}.
        BSONObjIterator keyIt(newKeyData.keyData);
        for (unsigned i = 0; i < _params.spec.numExtraBefore(); i++) {
            keyIt.next();
        }

        keyIt.next(); // Skip past 'term'.

        BSONElement scoreElement = keyIt.next();
        double documentTermScore = scoreElement.number();

        // Aggregate relevance score, term keys.
        *documentAggregateScore += documentTermScore;
        return NEED_TIME;
    }

}  // namespace mongo
