/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <boost/core/checked_delete.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/std_fwd.hpp"
#include "mongo/base/string_data.h"
#include "mongo/db/geo/r2_region_coverer.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <ostream>

#include <r1interval.h>
#include <s2cellid.h>
#include <s2edgeindex.h>

#include <boost/smart_ptr/scoped_ptr.hpp>
#include <util/math/vector3-inl.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using std::less;

// Definition
int const R2RegionCoverer::kDefaultMaxCells = 8;

// We define our own own comparison function on QueueEntries in order to
// make the results deterministic.  Using the default less<QueueEntry>,
// entries of equal priority would be sorted according to the memory address
// of the candidate.
struct R2RegionCoverer::CompareQueueEntries : public less<QueueEntry> {
    bool operator()(QueueEntry const& x, QueueEntry const& y) {
        return x.first < y.first;
    }
};

R2RegionCoverer::R2RegionCoverer(std::unique_ptr<GeoHashConverter> hashConverter)
    : _hashConverter(std::move(hashConverter)),
      _minLevel(0u),
      _maxLevel(GeoHash::kMaxBits),
      _maxCells(kDefaultMaxCells),
      _region(nullptr),
      _candidateQueue(new CandidateQueue),
      _results(new vector<GeoHash>) {}

// Need to declare explicitly because of scoped pointers.
R2RegionCoverer::~R2RegionCoverer() {}

void R2RegionCoverer::setMinLevel(unsigned int minLevel) {
    dassert(minLevel <= GeoHash::kMaxBits);
    _minLevel = min(GeoHash::kMaxBits, minLevel);
}

void R2RegionCoverer::setMaxLevel(unsigned int maxLevel) {
    dassert(maxLevel <= GeoHash::kMaxBits);
    _maxLevel = min(GeoHash::kMaxBits, maxLevel);
}

void R2RegionCoverer::setMaxCells(int maxCells) {
    _maxCells = maxCells;
}

void R2RegionCoverer::getCovering(const R2Region& region, vector<GeoHash>* cover) {
    // Strategy: Start with the full plane. Discard any
    // that do not intersect the shape.  Then repeatedly choose the
    // largest cell that intersects the shape and subdivide it.
    //
    // _result contains the cells that will be part of the output, while the
    // queue contains cells that we may still subdivide further.  Cells that
    // are entirely contained within the region are immediately added to the
    // output, while cells that do not intersect the region are immediately
    // discarded. Therefore the queue only contains cells that partially
    // intersect the region. Candidates are prioritized first according to
    // cell size (larger cells first), then by the number of intersecting
    // children they have (fewest children first), and then by the number of
    // fully contained children (fewest children first).

    MONGO_verify(_minLevel <= _maxLevel);
    dassert(_candidateQueue->empty());
    dassert(_results->empty());
    _region = &region;

    getInitialCandidates();

    while (!_candidateQueue->empty()) {
        Candidate* candidate = _candidateQueue->top().second;  // Owned
        _candidateQueue->pop();
        // REDACT?? I think this may have User info, but I'm not sure how to redact
        LOGV2_DEBUG(20637,
                    3,
                    "Pop: {candidate_cell}",
                    "candidate_cell"_attr = redact(candidate->cell.toString()));

        // Try to expand this cell into its children
        if (candidate->cell.getBits() < _minLevel || candidate->numChildren == 1 ||
            (int)_results->size() + (int)_candidateQueue->size() + candidate->numChildren <=
                _maxCells) {
            for (int i = 0; i < candidate->numChildren; i++) {
                addCandidate(candidate->children[i]);
            }
            deleteCandidate(candidate, false);
        } else {
            // Reached max cells. Move all candidates from the queue into results.
            candidate->isTerminal = true;
            addCandidate(candidate);
        }
        LOGV2_DEBUG(20638,
                    3,
                    "Queue: {candidateQueue_size}",
                    "candidateQueue_size"_attr = _candidateQueue->size());
    }

    _region = nullptr;
    cover->swap(*_results);
}

const GeoHashConverter& R2RegionCoverer::getHashConverter() const {
    return *_hashConverter;
}

// Caller owns the returned pointer
R2RegionCoverer::Candidate* R2RegionCoverer::newCandidate(const GeoHash& cell) {
    // Exclude the cell that doesn't intersect with the geometry.
    Box box = _hashConverter->unhashToBoxCovering(cell);

    if (_region->fastDisjoint(box)) {
        return nullptr;
    }

    Candidate* candidate = new Candidate();
    candidate->cell = cell;
    candidate->numChildren = 0;
    // Stop subdivision when we reach the max level or there is no need to do so.
    // Don't stop if we haven't reach min level.
    candidate->isTerminal =
        cell.getBits() >= _minLevel && (cell.getBits() >= _maxLevel || _region->fastContains(box));

    return candidate;
}

// Takes ownership of "candidate"
void R2RegionCoverer::addCandidate(Candidate* candidate) {
    if (candidate == nullptr)
        return;

    if (candidate->isTerminal) {
        _results->push_back(candidate->cell);
        deleteCandidate(candidate, true);
        return;
    }

    MONGO_verify(candidate->numChildren == 0);

    // Expand children
    int numTerminals = expandChildren(candidate);

    if (candidate->numChildren == 0) {
        deleteCandidate(candidate, true);
    } else if (numTerminals == 4 && candidate->cell.getBits() >= _minLevel) {
        // Optimization: add the parent cell rather than all of its children.
        candidate->isTerminal = true;
        addCandidate(candidate);
    } else {
        // Add the cell into the priority queue for further subdivision.
        //
        // We negate the priority so that smaller absolute priorities are returned
        // first.  The heuristic is designed to refine the largest cells first,
        // since those are where we have the largest potential gain.  Among cells
        // at the same level, we prefer the cells with the smallest number of
        // intersecting children.  Finally, we prefer cells that have the smallest
        // number of children that cannot be refined any further.
        int priority = -(((((int)candidate->cell.getBits() << 4) + candidate->numChildren) << 4) +
                         numTerminals);
        _candidateQueue->push(make_pair(priority, candidate));  // queue owns candidate
        // REDACT??
        LOGV2_DEBUG(20639,
                    3,
                    "Push: {candidate_cell} ({priority}) ",
                    "candidate_cell"_attr = redact(candidate->cell.toString()),
                    "priority"_attr = priority);
    }
}

// Dones't take ownership of "candidate"
int R2RegionCoverer::expandChildren(Candidate* candidate) {
    GeoHash childCells[4];
    tassert(9911943, "", candidate->cell.subdivide(childCells));

    int numTerminals = 0;
    for (int i = 0; i < 4; ++i) {
        Candidate* child = newCandidate(childCells[i]);
        if (child) {
            candidate->children[candidate->numChildren++] = child;
            if (child->isTerminal)
                ++numTerminals;
        }
    }
    return numTerminals;
}

// Takes ownership of "candidate"
void R2RegionCoverer::deleteCandidate(Candidate* candidate, bool freeChildren) {
    if (freeChildren) {
        for (int i = 0; i < candidate->numChildren; i++) {
            deleteCandidate(candidate->children[i], true);
        }
    }

    delete candidate;
}

void R2RegionCoverer::getInitialCandidates() {
    // Add the full plane
    // TODO a better initialization.
    addCandidate(newCandidate(GeoHash()));
}

//
// R2CellUnion
//
void R2CellUnion::init(const vector<GeoHash>& cellIds) {
    _cellIds = cellIds;
    normalize();
}

void R2CellUnion::add(const std::vector<GeoHash>& cellIds) {
    _cellIds.insert(_cellIds.end(), cellIds.begin(), cellIds.end());
    normalize();
}

void R2CellUnion::detach(std::vector<GeoHash>* cellIds) {
    _cellIds.swap(*cellIds);
    _cellIds.clear();
}

bool R2CellUnion::contains(const GeoHash cellId) const {
    // Since all cells are ordered, if an ancestor of id exists, it must be the previous one.
    vector<GeoHash>::const_iterator it;
    it = std::upper_bound(_cellIds.begin(), _cellIds.end(), cellId);  // it > cellId
    return it != _cellIds.begin() && (--it)->contains(cellId);        // --it <= cellId
}

bool R2CellUnion::normalize() {
    vector<GeoHash> output;
    output.reserve(_cellIds.size());
    sort(_cellIds.begin(), _cellIds.end());

    for (size_t i = 0; i < _cellIds.size(); i++) {
        GeoHash id = _cellIds[i];

        // Parent is less than children. If an ancestor of id exists, it must be the last one.
        //
        // Invariant: output doesn't contain intersected cells (ancestor and its descendants)
        // Proof: Assume another cell "c" exists between ancestor "p" and the current "id",
        // i.e. p < c < id, then "c" has "p" as its prefix, since id share the same prefix "p",
        // so "p" contains "c", which conflicts with the invariant.
        if (!output.empty() && output.back().contains(id))
            continue;

        // Check whether the last 3 elements of "output" plus "id" can be
        // collapsed into a single parent cell.
        while (output.size() >= 3) {
            // A necessary (but not sufficient) condition is that the XOR of the
            // four cells must be zero.  This is also very fast to test.
            if ((output.end()[-3].getHash() ^ output.end()[-2].getHash() ^
                 output.back().getHash()) != id.getHash())
                break;

            // Now we do a slightly more expensive but exact test.
            GeoHash parent = id.parent();
            if (parent != output.end()[-3].parent() || parent != output.end()[-2].parent() ||
                parent != output.end()[-1].parent())
                break;

            // Replace four children by their parent cell.
            output.erase(output.end() - 3, output.end());
            id = parent;
        }
        output.push_back(id);
    }
    if (output.size() < _cellIds.size()) {
        _cellIds.swap(output);
        return true;
    }
    return false;
}

string R2CellUnion::toString() const {
    std::stringstream ss;
    ss << "[ ";
    for (size_t i = 0; i < _cellIds.size(); i++) {
        ss << _cellIds[i] << " ";
    }
    ss << "]";
    return ss.str();
}

bool R2CellUnion::intersects(const GeoHash cellId) const {
    // After normalization, the cells will be ordered.
    // cellId intersects with the union if and only if it either contains or is contained by
    // a member of the union.
    std::vector<GeoHash>::const_iterator i =
        std::lower_bound(_cellIds.begin(), _cellIds.end(), cellId);
    if (i != _cellIds.end() && cellId.contains(*i)) {
        return true;
    }
    return i != _cellIds.begin() && (--i)->contains(cellId);
}

namespace {
void getDifferenceInternal(GeoHash cellId,
                           R2CellUnion const& cellUnion,
                           std::vector<GeoHash>* cellIds) {
    // Add the difference between cell and cellUnion to cellIds.
    // If they intersect but the difference is non-empty, divides and conquers.
    if (!cellUnion.intersects(cellId)) {
        cellIds->push_back(cellId);
    } else if (!cellUnion.contains(cellId)) {
        GeoHash children[4];
        if (cellId.subdivide(children)) {
            for (int i = 0; i < 4; i++) {
                getDifferenceInternal(children[i], cellUnion, cellIds);
            }
        }
    }
}
}  // namespace

void R2CellUnion::getDifference(const R2CellUnion& cellUnion) {
    std::vector<GeoHash> diffCellIds;
    for (size_t i = 0; i < _cellIds.size(); ++i) {
        getDifferenceInternal(_cellIds[i], cellUnion, &diffCellIds);
    }
    _cellIds.swap(diffCellIds);
}

} /* namespace mongo */
