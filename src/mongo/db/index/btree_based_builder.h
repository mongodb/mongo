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
