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

#include "mongo/db/exec/text.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"

namespace mongo {

    TextStage::TextStage(const TextStageParams& params,
                         WorkingSet* ws,
                         const MatchExpression* filter)
        : _params(params),
          _ftsMatcher(params.query, params.spec),
          _ws(ws),
          _filter(filter),
          _filledOutResults(false),
          _curResult(0) { }

    TextStage::~TextStage() { }

    bool TextStage::isEOF() {
        // If we haven't filled out our results yet we can't be EOF.
        if (!_filledOutResults) { return false; }

        // We're EOF when we've returned all our results.
        return _curResult >= _results.size();
    }

    PlanStage::StageState TextStage::work(WorkingSetID* out) {
        ++_commonStats.works;
        if (isEOF()) { return PlanStage::IS_EOF; }

        // Fill out our result queue.
        if (!_filledOutResults) {
            PlanStage::StageState ss = fillOutResults();
            if (ss == PlanStage::IS_EOF || ss == PlanStage::FAILURE) {
                return ss;
            }
            verify(ss == PlanStage::NEED_TIME);
        }

        // Having cached all our results, return them one at a time.

        // Fill out a WSM.
        WorkingSetID id = _ws->allocate();
        WorkingSetMember* member = _ws->get(id);
        member->loc = _results[_curResult].loc;
        member->obj = member->loc.obj();
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        member->addComputed(new TextScoreComputedData(_results[_curResult].score));

        // Advance to next result.
        ++_curResult;
        *out = id;
        return PlanStage::ADVANCED;
    }

    void TextStage::prepareToYield() {
        ++_commonStats.yields;
        // TODO: When we incrementally read results, tell our sub-runners to yield.
    }

    void TextStage::recoverFromYield() {
        ++_commonStats.unyields;
        // TODO: When we incrementally read results, tell our sub-runners to unyield.
    }

    void TextStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;
        // TODO: This is much slower than it should be.
        for (size_t i = 0; i < _results.size(); ++i) {
            if (dl == _results[i].loc) {
                _results.erase(_results.begin() + i);
                return;
            }
        }
    }

    PlanStageStats* TextStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_TEXT));
        ret->specific.reset(new TextStats(_specificStats));
        return ret.release();
    }

    PlanStage::StageState TextStage::fillOutResults() {
        Database* db = cc().database();
        Collection* collection = db->getCollection( _params.ns );
        if (NULL == collection) {
            warning() << "TextStage params namespace error";
            return PlanStage::FAILURE;
        }
        vector<int> idxMatches;
        collection->details()->findIndexByType("text", idxMatches);
        if (1 != idxMatches.size()) {
            warning() << "Expected exactly one text index";
            return PlanStage::FAILURE;
        }

        // Get all the index scans for each term in our query.
        vector<IndexScan*> scanners;
        for (size_t i = 0; i < _params.query.getTerms().size(); i++) {
            const string& term = _params.query.getTerms()[i];
            IndexScanParams params;
            params.bounds.startKey = FTSIndexFormat::getIndexKey(MAX_WEIGHT, term,
                                                                 _params.indexPrefix);
            params.bounds.endKey = FTSIndexFormat::getIndexKey(0, term, _params.indexPrefix);
            params.bounds.endKeyInclusive = true;
            params.bounds.isSimpleRange = true;
            params.descriptor = collection->getIndexCatalog()->getDescriptor(idxMatches[0]);
            params.forceBtreeAccessMethod = true;
            params.direction = -1;
            IndexScan* ixscan = new IndexScan(params, _ws, NULL);
            scanners.push_back(ixscan);
        }

        // For each index scan, read all results and store scores.
        size_t currentIndexScanner = 0;
        while (currentIndexScanner < scanners.size()) {
            BSONObj keyObj;
            DiskLoc loc;

            WorkingSetID id;
            PlanStage::StageState state = scanners[currentIndexScanner]->work(&id);

            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* wsm = _ws->get(id);
                IndexKeyDatum& keyDatum = wsm->keyData.back();
                filterAndScore(keyDatum.keyData, wsm->loc);
                _ws->free(id);
            }
            else if (PlanStage::IS_EOF == state) {
                // Done with this scan.
                ++currentIndexScanner;
            }
            else if (PlanStage::NEED_FETCH == state) {
                // We're calling work() on ixscans and they have no way to return a fetch.
                verify(false);
            }
            else if (PlanStage::NEED_TIME == state) {
                // We are a blocking stage, so ignore scanner's request for more time.
            }
            else {
                verify(PlanStage::FAILURE == state);
                warning() << "error from index scan during text stage: invalid FAILURE state";
                for (size_t i=0; i<scanners.size(); ++i) { delete scanners[i]; }
                return PlanStage::FAILURE;
            }
        }

        for (size_t i=0; i<scanners.size(); ++i) { delete scanners[i]; }

        // Filter for phrases and negative terms, score and truncate.
        for (ScoreMap::iterator i = _scores.begin(); i != _scores.end(); ++i) {
            DiskLoc loc = i->first;
            double score = i->second;

            // Ignore non-matched documents.
            if (score < 0) {
                continue;
            }

            // Filter for phrases and negated terms
            if (_params.query.hasNonTermPieces()) {
                if (!_ftsMatcher.matchesNonTerm(loc.obj())) {
                    continue;
                }
            }

            _results.push_back(ScoredLocation(loc, score));
        }

        _filledOutResults = true;

        if (_results.size() == 0) {
            return PlanStage::IS_EOF;
        }
        return PlanStage::NEED_TIME;
    }

    class TextMatchableDocument : public MatchableDocument {
    public:
        TextMatchableDocument(const BSONObj& keyPattern, const BSONObj& key, DiskLoc loc, bool *fetched)
            : _keyPattern(keyPattern),
              _key(key),
              _loc(loc),
              _fetched(fetched) { }

        BSONObj toBSON() const {
            *_fetched = true;
            return _loc.obj();
        }

        virtual ElementIterator* allocateIterator(const ElementPath* path) const {
            BSONObjIterator keyPatternIt(_keyPattern);
            BSONObjIterator keyDataIt(_key);

            // Look in the key.
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

            // All else fails, fetch.
            *_fetched = true;
            return new BSONElementIterator(path, _loc.obj());
        }

        virtual void releaseIterator( ElementIterator* iterator ) const {
            delete iterator;
        }

    private:
        BSONObj _keyPattern;
        BSONObj _key;
        DiskLoc _loc;
        bool* _fetched;
    };

    void TextStage::filterAndScore(BSONObj key, DiskLoc loc) {
        ++_specificStats.keysExamined;

        // Locate score within possibly compound key: {prefix,term,score,suffix}.
        BSONObjIterator keyIt(key);
        for (unsigned i = 0; i < _params.spec.numExtraBefore(); i++) {
            keyIt.next();
        }

        keyIt.next(); // Skip past 'term'.

        BSONElement scoreElement = keyIt.next();
        double documentTermScore = scoreElement.number();
        double& documentAggregateScore = _scores[loc];
        
        // Handle filtering.
        if (documentAggregateScore < 0) {
            // We have already rejected this document.
            return;
        }

        if (documentAggregateScore == 0) {
            if (_filter) {
                // We have not seen this document before and need to apply a filter.
                bool fetched = false;
                TextMatchableDocument tdoc(_params.index->keyPattern(), key, loc, &fetched);

                if (!_filter->matches(&tdoc)) {
                    // We had to fetch but we're not going to return it.
                    if (fetched) {
                        ++_specificStats.fetches;
                    }
                    documentAggregateScore = -1;
                    return;
                }
            }
            else {
                // If we're here, we're going to return the doc, and we do a fetch later.
                ++_specificStats.fetches;
            }
        }

        // Aggregate relevance score, term keys.
        documentAggregateScore += documentTermScore;
    }

}  // namespace mongo
