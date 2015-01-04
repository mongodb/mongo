// record_store_test_manyiter.cpp

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

#include "mongo/db/storage/record_store_test_harness.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace mongo {

    using boost::scoped_ptr;

    // Create multiple iterators over an empty record store.
    TEST( RecordStoreTestHarness, GetManyIteratorsEmpty ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            vector<RecordIterator*> v = rs->getManyIterators( opCtx.get() );

            for (vector<RecordIterator*>::iterator vIter = v.begin();
                 vIter != v.end(); vIter++) {

                RecordIterator *rIter = *vIter;
                ASSERT( rIter->isEOF() );
                ASSERT_EQUALS( RecordId(), rIter->curr() );
                ASSERT_EQUALS( RecordId(), rIter->getNext() );
                ASSERT( rIter->isEOF() );
                ASSERT_EQUALS( RecordId(), rIter->curr() );

                delete rIter;
            }
        }
    }

    // Create multiple iterators over a nonempty record store.
    TEST( RecordStoreTestHarness, GetManyIteratorsNonEmpty ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        const int nToInsert = 10;
        RecordId locs[nToInsert];
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                stringstream ss;
                ss << "record " << i;
                string data = ss.str();

                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<RecordId> res = rs->insertRecord( opCtx.get(),
                                                            data.c_str(),
                                                            data.size() + 1,
                                                            false );
                ASSERT_OK( res.getStatus() );
                locs[i] = res.getValue();
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, rs->numRecords( opCtx.get() ) );
        }

        set<RecordId> remain( locs, locs + nToInsert );
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            vector<RecordIterator*> v = rs->getManyIterators( opCtx.get() );

            for (vector<RecordIterator*>::iterator vIter = v.begin();
                 vIter != v.end(); vIter++) {

                RecordIterator *rIter = *vIter;
                while ( !rIter->isEOF() ) {
                    RecordId loc = rIter->curr();
                    ASSERT( 1 == remain.erase( loc ) );
                    ASSERT_EQUALS( loc, rIter->getNext() );
                }

                ASSERT_EQUALS( RecordId(), rIter->curr() );
                ASSERT_EQUALS( RecordId(), rIter->getNext() );
                ASSERT( rIter->isEOF() );
                ASSERT_EQUALS( RecordId(), rIter->curr() );

                delete rIter;
            }
            ASSERT( remain.empty() );
        }
    }

} // namespace mongo
