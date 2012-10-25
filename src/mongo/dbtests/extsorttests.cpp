//@file extsorttests.cpp : mongo/db/extsort.{h,cpp} tests

/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/extsort.h"

#include "mongo/db/pdfile.h"
#include "mongo/platform/cstdint.h"

#include "mongo/dbtests/dbtests.h"

namespace ExtSortTests {

    static const char* const _ns = "unittests.extsort";
    DBDirectClient _client;

    /** BSONObjExternalSorter::sort() sorts the keys provided to add(). */
    class Sort {
    public:
        void run() {
            // Create a sorter.
            BSONObjExternalSorter sorter( IndexInterface::defaultVersion(), BSON( "a" << 1 ) );
            // Add keys to the sorter.
            int32_t nDocs = 130;
            for( int32_t i = 0; i < nDocs; ++i ) {
                // Insert values in reverse order, for subsequent sort.
                sorter.add( BSON( "" << ( nDocs - 1 - i ) ), /* dummy disk loc */ DiskLoc(), true );
            }
            // The sorter's footprint is now positive.
            ASSERT( sorter.getCurSizeSoFar() > 0 );
            // Sort the keys.
            sorter.sort( true );
            // Check that the keys have been sorted.
            auto_ptr<BSONObjExternalSorter::Iterator> iterator = sorter.iterator();
            int32_t expectedKey = 0;
            while( iterator->more() ) {
                ASSERT_EQUALS( BSON( "" << expectedKey++ ), iterator->next().first );
            }
            ASSERT_EQUALS( nDocs, expectedKey );
        }
    };

    /**
     * BSONObjExternalSorter::add() aborts if the current operation is interrupted, even if storage
     * system writes have occurred.
     */
    class InterruptAdd {
    public:
        InterruptAdd( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            _client.createCollection( _ns );
            // Take a write lock.
            Client::WriteContext ctx( _ns );
            // Do a write to ensure the implementation will interrupt sort() even after a write has
            // occurred.
            BSONObj newDoc;
            theDataFileMgr.insertWithObjMod( _ns, newDoc );
            // Create a sorter with a max file size of only 10k, to trigger a file flush after a
            // relatively small number of inserts.
            BSONObjExternalSorter sorter( IndexInterface::defaultVersion(),
                                          BSON( "a" << 1 ),
                                          10 * 1024 );
            // Register a request to kill the current operation.
            cc().curop()->kill();
            if ( _mayInterrupt ) {
                // When enough keys are added to fill the first file, an interruption will be
                // triggered as the records are sorted for the file.
                ASSERT_THROWS( addKeysUntilFileFlushed( &sorter, _mayInterrupt ), UserException );
            }
            else {
                // When enough keys are added to fill the first file, an interruption when the
                // records are sorted for the file is prevented because mayInterrupt == false.
                addKeysUntilFileFlushed( &sorter, _mayInterrupt );
            }
        }
    private:
        static void addKeysUntilFileFlushed( BSONObjExternalSorter* sorter, bool mayInterrupt ) {
            while( sorter->numFiles() == 0 ) {
                sorter->add( BSON( "" << 1 ), /* dummy disk loc */ DiskLoc(), mayInterrupt );
            }
        }
        bool _mayInterrupt;
    };

    /**
     * BSONObjExternalSorter::sort() aborts if the current operation is interrupted, even if storage
     * system writes have occurred.
     */
    class InterruptSort {
    public:
        InterruptSort( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            _client.createCollection( _ns );
            // Take a write lock.
            Client::WriteContext ctx( _ns );
            // Do a write to ensure the implementation will interrupt sort() even after a write has
            // occurred.
            BSONObj newDoc;
            theDataFileMgr.insertWithObjMod( _ns, newDoc );
            // Create a sorter.
            BSONObjExternalSorter sorter( IndexInterface::defaultVersion(), BSON( "a" << 1 ) );
            // Add keys to the sorter.
            int32_t nDocs = 130;
            for( int32_t i = 0; i < nDocs; ++i ) {
                sorter.add( BSON( "" << i ), /* dummy disk loc */ DiskLoc(), false );
            }
            ASSERT( sorter.getCurSizeSoFar() > 0 );
            // Register a request to kill the current operation.
            cc().curop()->kill();
            if ( _mayInterrupt ) {
                // The sort is aborted due to the kill request.
                ASSERT_THROWS( sorter.sort( _mayInterrupt ), UserException );
                // TODO Check that an iterator cannot be retrieved because the keys are unsorted (Not
                // currently implemented.)
                if ( 0 ) {
                    ASSERT_THROWS( sorter.iterator(), UserException );
                }
            }
            else {
                // Sort the keys.
                sorter.sort( _mayInterrupt );
                // Check that the keys have been sorted.
                auto_ptr<BSONObjExternalSorter::Iterator> iterator = sorter.iterator();
                int32_t expectedKey = 0;
                while( iterator->more() ) {
                    ASSERT_EQUALS( BSON( "" << expectedKey++ ), iterator->next().first );
                }
                ASSERT_EQUALS( nDocs, expectedKey );
            }
        }
    private:
        bool _mayInterrupt;
    };

    class ExtSortTests : public Suite {
    public:
        ExtSortTests() :
            Suite( "extsort" ) {
        }

        void setupTests() {
            add<Sort>();
            add<InterruptAdd>( false );
            add<InterruptAdd>( true );
            add<InterruptSort>( false );
            add<InterruptSort>( true );
        }
    } extSortTests;

} // namespace ExtSortTests
