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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "mongo/db/query/plan_ranker.h"

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace {

/**
 * Comparator for (scores, candidateIndex) in pickBestPlan().
 */
bool scoreComparator(const std::pair<double, size_t>& lhs, const std::pair<double, size_t>& rhs) {
    // Just compare score in lhs.first and rhs.first;
    // Ignore candidate array index in lhs.second and rhs.second.
    return lhs.first > rhs.first;
}

}  // namespace

namespace mongo {

using std::endl;
using std::vector;

// static
size_t PlanRanker::pickBestPlan(const vector<CandidatePlan>& candidates, PlanRankingDecision* why) {
    invariant(!candidates.empty());
    invariant(why);

    // A plan that hits EOF is automatically scored above
    // its peers. If multiple plans hit EOF during the same
    // set of round-robin calls to work(), then all such plans
    // receive the bonus.
    double eofBonus = 1.0;

    // Each plan will have a stat tree.
    vector<PlanStageStats*> statTrees;

    // Get stat trees from each plan.
    // Copy stats trees instead of transferring ownership
    // because multi plan runner will need its own stats
    // trees for explain.
    for (size_t i = 0; i < candidates.size(); ++i) {
        statTrees.push_back(candidates[i].root->getStats().release());
    }

    // Holds (score, candidateInndex).
    // Used to derive scores and candidate ordering.
    vector<std::pair<double, size_t>> scoresAndCandidateindices;

    // Compute score for each tree.  Record the best.
    for (size_t i = 0; i < statTrees.size(); ++i) {
        LOG(5) << "Scoring plan " << i << ":" << endl
               << candidates[i].solution->toString() << "Stats:\n"
               << Explain::statsToBSON(*statTrees[i]).jsonString(Strict, true);
        LOG(2) << "Scoring query plan: " << Explain::getPlanSummary(candidates[i].root)
               << " planHitEOF=" << statTrees[i]->common.isEOF;

        double score = scoreTree(statTrees[i]);
        LOG(5) << "score = " << score << endl;
        if (statTrees[i]->common.isEOF) {
            LOG(5) << "Adding +" << eofBonus << " EOF bonus to score." << endl;
            score += 1;
        }
        scoresAndCandidateindices.push_back(std::make_pair(score, i));
    }

    // Sort (scores, candidateIndex). Get best child and populate candidate ordering.
    std::stable_sort(
        scoresAndCandidateindices.begin(), scoresAndCandidateindices.end(), scoreComparator);

    // Determine whether plans tied for the win.
    if (scoresAndCandidateindices.size() > 1U) {
        double bestScore = scoresAndCandidateindices[0].first;
        double runnerUpScore = scoresAndCandidateindices[1].first;
        const double epsilon = 1e-10;
        why->tieForBest = std::abs(bestScore - runnerUpScore) < epsilon;
    }

    // Update results in 'why'
    // Stats and scores in 'why' are sorted in descending order by score.
    why->stats.clear();
    why->scores.clear();
    why->candidateOrder.clear();
    for (size_t i = 0; i < scoresAndCandidateindices.size(); ++i) {
        double score = scoresAndCandidateindices[i].first;
        size_t candidateIndex = scoresAndCandidateindices[i].second;

        // We shouldn't cache the scores with the EOF bonus included,
        // as this is just a tie-breaking measure for plan selection.
        // Plans not run through the multi plan runner will not receive
        // the bonus.
        //
        // An example of a bad thing that could happen if we stored scores
        // with the EOF bonus included:
        //
        //   Let's say Plan A hits EOF, is the highest ranking plan, and gets
        //   cached as such. On subsequent runs it will not receive the bonus.
        //   Eventually the plan cache feedback mechanism will evict the cache
        //   entry---the scores will appear to have fallen due to the missing
        //   EOF bonus.
        //
        // This begs the question, why don't we include the EOF bonus in
        // scoring of cached plans as well? The problem here is that the cached
        // plan runner always runs plans to completion before scoring. Queries
        // that don't get the bonus in the multi plan runner might get the bonus
        // after being run from the plan cache.
        if (statTrees[candidateIndex]->common.isEOF) {
            score -= eofBonus;
        }

        why->stats.mutableVector().push_back(statTrees[candidateIndex]);
        why->scores.push_back(score);
        why->candidateOrder.push_back(candidateIndex);
    }

    size_t bestChild = scoresAndCandidateindices[0].second;
    return bestChild;
}

// TODO: Move this out.  This is a signal for ranking but will become its own complicated
// stats-collecting beast.
double computeSelectivity(const PlanStageStats* stats) {
    if (STAGE_IXSCAN == stats->stageType) {
        IndexScanStats* iss = static_cast<IndexScanStats*>(stats->specific.get());
        return iss->keyPattern.nFields();
    } else {
        double sum = 0;
        for (size_t i = 0; i < stats->children.size(); ++i) {
            sum += computeSelectivity(stats->children[i].get());
        }
        return sum;
    }
}

bool hasStage(const StageType type, const PlanStageStats* stats) {
    if (type == stats->stageType) {
        return true;
    }
    for (size_t i = 0; i < stats->children.size(); ++i) {
        if (hasStage(type, stats->children[i].get())) {
            return true;
        }
    }
    return false;
}

// static
double PlanRanker::scoreTree(const PlanStageStats* stats) {
    // We start all scores at 1.  Our "no plan selected" score is 0 and we want all plans to
    // be greater than that.
    double baseScore = 1;

    // How many "units of work" did the plan perform. Each call to work(...)
    // counts as one unit.
    size_t workUnits = stats->common.works;
    invariant(workUnits != 0);

    // How much did a plan produce?
    // Range: [0, 1]
    double productivity =
        static_cast<double>(stats->common.advanced) / static_cast<double>(workUnits);

    // Just enough to break a tie. Must be small enough to ensure that a more productive
    // plan doesn't lose to a less productive plan due to tie breaking.
    const double epsilon = std::min(1.0 / static_cast<double>(10 * workUnits), 1e-4);

    // We prefer covered projections.
    //
    // We only do this when we have a projection stage because we have so many jstests that
    // check bounds even when a collscan plan is just as good as the ixscan'd plan :(
    double noFetchBonus = epsilon;
    if (hasStage(STAGE_PROJECTION, stats) && hasStage(STAGE_FETCH, stats)) {
        noFetchBonus = 0;
    }

    // In the case of ties, prefer solutions without a blocking sort
    // to solutions with a blocking sort.
    double noSortBonus = epsilon;
    if (hasStage(STAGE_SORT, stats)) {
        noSortBonus = 0;
    }

    // In the case of ties, prefer single index solutions to ixisect. Index
    // intersection solutions are often slower than single-index solutions
    // because they require examining a superset of index keys that would be
    // examined by a single index scan.
    //
    // On the other hand, index intersection solutions examine the same
    // number or fewer of documents. In the case that index intersection
    // allows us to examine fewer documents, the penalty given to ixisect
    // can be made up via the no fetch bonus.
    double noIxisectBonus = epsilon;
    if (hasStage(STAGE_AND_HASH, stats) || hasStage(STAGE_AND_SORTED, stats)) {
        noIxisectBonus = 0;
    }

    double tieBreakers = noFetchBonus + noSortBonus + noIxisectBonus;
    double score = baseScore + productivity + tieBreakers;

    mongoutils::str::stream ss;
    ss << "score(" << score << ") = baseScore(" << baseScore << ")"
       << " + productivity((" << stats->common.advanced << " advanced)/(" << stats->common.works
       << " works) = " << productivity << ")"
       << " + tieBreakers(" << noFetchBonus << " noFetchBonus + " << noSortBonus
       << " noSortBonus + " << noIxisectBonus << " noIxisectBonus = " << tieBreakers << ")";
    std::string scoreStr = ss;
    LOG(2) << scoreStr;

    if (internalQueryForceIntersectionPlans) {
        if (hasStage(STAGE_AND_HASH, stats) || hasStage(STAGE_AND_SORTED, stats)) {
            // The boost should be >2.001 to make absolutely sure the ixisect plan will win due
            // to the combination of 1) productivity, 2) eof bonus, and 3) no ixisect bonus.
            score += 3;
            LOG(5) << "Score boosted to " << score << " due to intersection forcing." << endl;
        }
    }

    return score;
}

}  // namespace mongo
