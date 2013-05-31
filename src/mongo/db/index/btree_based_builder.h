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
*/

#pragma once

#include <set>

#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"

namespace IndexUpdateTests {
    class AddKeysToPhaseOne;
    class InterruptAddKeysToPhaseOne;
    class DoDropDups;
    class InterruptDoDropDups;
}

namespace mongo {

    class BSONObjExternalSorter;
    class ExternalSortComparison;
    class IndexDetails;
    class NamespaceDetails;
    class ProgressMeter;
    class ProgressMeterHolder;
    struct SortPhaseOne;

    class BtreeBasedBuilder {
    public:
        /**
         * Want to build an index?  Call this.  Throws DBException.
         */
        static uint64_t fastBuildIndex(const char* ns, NamespaceDetails* d, IndexDetails& idx,
                                       bool mayInterrupt, int idxNo);
        static DiskLoc makeEmptyIndex(const IndexDetails& idx);
        static ExternalSortComparison* getComparison(int version, const BSONObj& keyPattern);

    private:
        friend class IndexUpdateTests::AddKeysToPhaseOne;
        friend class IndexUpdateTests::InterruptAddKeysToPhaseOne;
        friend class IndexUpdateTests::DoDropDups;
        friend class IndexUpdateTests::InterruptDoDropDups;


        static void addKeysToPhaseOne(NamespaceDetails* d, const char* ns, const IndexDetails& idx,
                                      const BSONObj& order, SortPhaseOne* phaseOne,
                                      int64_t nrecords, ProgressMeter* progressMeter,
                                      bool mayInterrupt,
                                      int idxNo);

        static void doDropDups(const char* ns, NamespaceDetails* d, const set<DiskLoc>& dupsToDrop,
                               bool mayInterrupt );
    };

    // Exposed for testing purposes.
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

}  // namespace mongo
