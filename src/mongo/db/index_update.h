/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#pragma once

#include "mongo/db/diskloc.h"
#include "mongo/db/index.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/cstdint.h"

namespace mongo {
    class NamespaceDetails;
    class Record;

    // unindex all keys in index for this record. 
    void unindexRecord(NamespaceDetails *d, Record *todelete, const DiskLoc& dl, bool noWarn = false);

    // Build an index in the foreground
    // If background is false, uses fast index builder
    // If background is true, uses background index builder; blocks until done.
    void buildAnIndex(const std::string& ns,
                      NamespaceDetails *d,
                      IndexDetails& idx,
                      bool mayInterrupt);

    // add index keys for a newly inserted record 
    // done in two steps/phases to allow potential deferal of write lock portion in the future
    void indexRecordUsingTwoSteps(const char *ns, NamespaceDetails *d, BSONObj obj,
                                         DiskLoc loc, bool shouldBeUnlocked);

    // Given an object, populate "inserter" with information necessary to update indexes.
    void fetchIndexInserters(BSONObjSet & /*out*/keys,
                             IndexInterface::IndexInserter &inserter,
                             NamespaceDetails *d,
                             int idxNo,
                             const BSONObj& obj,
                             DiskLoc recordLoc,
                             const bool allowDups = false);

    bool dropIndexes( NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder, bool maydeleteIdIndex );

    /**
     * Add an _id index to namespace @param 'ns' if not already present.
     * @param mayInterrupt When true, killop may interrupt the function call.
     */
    void ensureHaveIdIndex(const char* ns, bool mayInterrupt);

    ////// The remaining functions are only included in this header file for unit testing.

    class BSONObjExternalSorter;
    class CurOp;
    class ProgressMeter;
    class ProgressMeterHolder;
    struct SortPhaseOne;
    class Timer;

    /** Extract index keys from the @param 'ns' to the external sorter in @param 'phaseOne'. */
    void addKeysToPhaseOne( const char* ns,
                            const IndexDetails& idx,
                            const BSONObj& order,
                            SortPhaseOne* phaseOne,
                            int64_t nrecords,
                            ProgressMeter* progressMeter,
                            bool mayInterrupt );

    /** Popuate the index @param 'idx' using the keys contained in @param 'sorter'. */
    template< class V >
    void buildBottomUpPhases2And3( bool dupsAllowed,
                                   IndexDetails& idx,
                                   BSONObjExternalSorter& sorter,
                                   bool dropDups,
                                   set<DiskLoc>& dupsToDrop,
                                   CurOp* op,
                                   SortPhaseOne* phase1,
                                   ProgressMeterHolder& pm,
                                   Timer& t,
                                   bool mayInterrupt );

    /** Drop duplicate documents from the set @param 'dupsToDrop'. */
    void doDropDups( const char* ns,
                     NamespaceDetails* d,
                     const set<DiskLoc>& dupsToDrop,
                     bool mayInterrupt );

} // namespace mongo
