/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>
#include <queue>
#include <vector>

#include "mongo/db/geo/hash.h"


namespace mongo {

class R2Region;

class R2RegionCoverer : boost::noncopyable {
    // By default, the covering uses at most 8 cells at any level.
    static const int kDefaultMaxCells;  // = 8;

public:
    R2RegionCoverer(GeoHashConverter* hashConverter);
    ~R2RegionCoverer();

    // Set the minimum and maximum cell level to be used.  The default is to use
    // all cell levels.  Requires: max_level() >= min_level().
    void setMinLevel(unsigned int minLevel);
    void setMaxLevel(unsigned int maxLevel);
    unsigned int minLevel() const {
        return _minLevel;
    }
    unsigned int maxLevel() const {
        return _maxLevel;
    }

    // Sets the maximum desired number of cells in the approximation (defaults
    // to kDefaultMaxCells).
    //
    // For any setting of max_cells(), an arbitrary number of cells may be
    // returned if min_level() is too high for the region being approximated.
    //
    // TODO(sz): accuracy experiments similar to S2RegionCoverer.
    void setMaxCells(int maxCells);
    int maxCells() const {
        return _maxCells;
    }

    void getCovering(const R2Region& region, std::vector<GeoHash>* cover);

private:
    struct Candidate {
        GeoHash cell;
        bool isTerminal;  // Cell should not be expanded further.
        int numChildren;  // Number of children that intersect the region.
        Candidate* children[4];
    };

    // If the cell intersects the given region, return a new candidate with no
    // children, otherwise return NULL.  Also marks the candidate as "terminal"
    // if it should not be expanded further.
    Candidate* newCandidate(GeoHash const& cell);

    // Process a candidate by either adding it to the result_ vector or
    // expanding its children and inserting it into the priority queue.
    // Passing an argument of NULL does nothing.
    void addCandidate(Candidate* candidate);

    // Free the memory associated with a candidate.
    void deleteCandidate(Candidate* candidate, bool freeChildren);

    // Populate the children of "candidate" by expanding from the given cell.
    // Returns the number of children that were marked "terminal".
    int expandChildren(Candidate* candidate);

    // Computes a set of initial candidates that cover the given region.
    void getInitialCandidates();

    GeoHashConverter* _hashConverter;  // Not owned.
    // min / max level as unsigned so as to be consistent with GeoHash
    unsigned int _minLevel;
    unsigned int _maxLevel;
    int _maxCells;

    // Save the copy of pointer temporarily to avoid passing this parameter internally.
    // Only valid for the duration of a single getCovering() call.
    R2Region const* _region;

    // We keep the candidates that may intersect with this region in a priority queue.
    struct CompareQueueEntries;
    typedef std::pair<int, Candidate*> QueueEntry;
    typedef std::priority_queue<QueueEntry, std::vector<QueueEntry>, CompareQueueEntries>
        CandidateQueue;
    boost::scoped_ptr<CandidateQueue> _candidateQueue;  // Priority queue owns candidate pointers.
    boost::scoped_ptr<std::vector<GeoHash>> _results;
};


// An R2CellUnion is a region consisting of cells of various sizes.
class R2CellUnion : boost::noncopyable {
public:
    void init(const std::vector<GeoHash>& cellIds);
    // Returns true if the cell union contains the given cell id.
    bool contains(const GeoHash cellId) const;
    // Return true if the cell union intersects the given cell id.
    bool intersects(const GeoHash cellId) const;
    std::string toString() const;

    // Direct access to the underlying vector.
    std::vector<GeoHash> const& cellIds() const {
        return _cellIds;
    }

    // Swaps _cellIds with the given vector of cellIds.
    void detach(std::vector<GeoHash>* cellIds);

    // Adds the cells to _cellIds and calls normalize().
    void add(const std::vector<GeoHash>& cellIds);

    // Subtracts cellUnion from *this
    void getDifference(const R2CellUnion& cellUnion);

private:
    // Normalizes the cell union by discarding cells that are contained by other
    // cells, replacing groups of 4 child cells by their parent cell whenever
    // possible, and sorting all the cell ids in increasing order.  Returns true
    // if the number of cells was reduced.
    bool normalize();
    std::vector<GeoHash> _cellIds;
};

} /* namespace mongo */
