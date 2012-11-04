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

#include "mongo/db/intervalbtreecursor.h"

#include "mongo/db/btree.h"
#include "mongo/db/pdfile.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/cstdint.h"

namespace IntervalBtreeCursorTests {

    DBDirectClient _client;
    const char* _ns = "unittests.intervalbtreecursor";

    /** An IntervalBtreeCursor can only be created for a version 1 index. */
    class WrongIndexVersion {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );

            // Create a version 0 index.
            _client.ensureIndex( _ns, BSON( "a" << 1 ), false, "", true, false, /* version */ 0 );

            // Attempt to create a cursor on the a:1 index.
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 5 ),
                                                                  true,
                                                                  BSON( "" << 6 ),
                                                                  true ) );

            // No cursor was created because the index was not of version 1.
            ASSERT( !cursor );
        }
    };

    class BasicAccessors {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            _client.insert( _ns, BSON( "a" << 5 ) );
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 5 ),
                                                                  true,
                                                                  BSON( "" << 6 ),
                                                                  true ) );

            // Create a reference BasicCursor, which will return identical values from certain
            // accessors.
            boost::shared_ptr<Cursor> reference = theDataFileMgr.findAll( _ns );

            ASSERT( cursor->ok() );
            ASSERT( reference->_current() == cursor->_current() );
            ASSERT_EQUALS( reference->current(), cursor->current() );
            ASSERT_EQUALS( reference->currLoc(), cursor->currLoc() );
            ASSERT_EQUALS( BSON( "" << 5 ), cursor->currKey() );
            ASSERT_EQUALS( cursor->currLoc(), cursor->refLoc() );
            ASSERT_EQUALS( BSON( "a" << 1 ), cursor->indexKeyPattern() );
            ASSERT( !cursor->supportGetMore() );
            ASSERT( cursor->supportYields() );
            ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
            ASSERT( !cursor->isMultiKey() );
            ASSERT( !cursor->modifiedKeys() );
            ASSERT_EQUALS( BSON( "lower" << BSON( "a" << 5 ) << "upper" << BSON( "a" << 6 ) ),
                           cursor->prettyIndexBounds() );

            // Advance the cursor to the end.
            ASSERT( !cursor->advance() );
            ASSERT( !cursor->ok() );
            ASSERT( cursor->currLoc().isNull() );
            ASSERT( cursor->currKey().isEmpty() );
            ASSERT( cursor->refLoc().isNull() );
        }
    };

    /**
     * Check nscanned counting semantics.  The expectation is to match the behavior of BtreeCursor,
     * as described in the test comments.
     */
    class Nscanned {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << 5 ) );
            }
            _client.insert( _ns, BSON( "a" << 7 ) );
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 5 ),
                                                                  true,
                                                                  BSON( "" << 6 ),
                                                                  true ) );
            // nscanned is 1 for the first match.
            ASSERT_EQUALS( 1, cursor->nscanned() );
            for( int32_t i = 1; i < 10; ++i ) {
                ASSERT( cursor->ok() );

                // nscanned is incremented by 1 for intermediate matches.
                ASSERT_EQUALS( i, cursor->nscanned() );
                ASSERT( cursor->advance() );
            }
            ASSERT( cursor->ok() );
            ASSERT_EQUALS( 10, cursor->nscanned() );
            ASSERT( !cursor->advance() );
            ASSERT( !cursor->ok() );

            // nscanned is not incremented when reaching the end of the cursor.
            ASSERT_EQUALS( 10, cursor->nscanned() );            
        }
    };

    /** Check that a CoveredIndexMatcher can be set and used properly by the cursor. */
    class Matcher {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );

            // Insert a document that will match.
            _client.insert( _ns, BSON( "a" << 5 << "b" << 1 ) );

            // Insert a document that will not match.
            _client.insert( _ns, BSON( "a" << 5 << "b" << 2 ) );
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 5 ),
                                                                  true,
                                                                  BSON( "" << 5 ),
                                                                  true ) );

            // No matcher is set yet.
            ASSERT( !cursor->matcher() );
            ASSERT( cursor->currentMatches() );

            // Create a matcher and set it on the cursor.
            boost::shared_ptr<CoveredIndexMatcher> matcher
                    ( new CoveredIndexMatcher( BSON( "a" << 5 << "b" << 1 ), BSON( "a" << 1 ) ) );
            cursor->setMatcher( matcher );

            // The document with b:1 matches.
            ASSERT_EQUALS( 1, cursor->current()[ "b" ].Int() );
            ASSERT( cursor->matcher()->matchesCurrent( cursor.get() ) );
            ASSERT( cursor->currentMatches() );
            cursor->advance();

            // The document with b:2 does not match.
            ASSERT_EQUALS( 2, cursor->current()[ "b" ].Int() );
            ASSERT( !cursor->matcher()->matchesCurrent( cursor.get() ) );
            ASSERT( !cursor->currentMatches() );
        }
    };

    /** Check that dups are properly identified by the cursor. */
    class Dups {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            _client.insert( _ns, BSON( "a" << BSON_ARRAY( 5 << 7 ) ) );
            _client.insert( _ns, BSON( "a" << BSON_ARRAY( 6 << 8 ) ) );
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 5 ),
                                                                  true,
                                                                  BSON( "" << 10 ),
                                                                  true ) );
            ASSERT( cursor->isMultiKey() );
            ASSERT( cursor->modifiedKeys() );

            // This is the 5,7 document, first time seen.  Not a dup.
            DiskLoc firstLoc = cursor->currLoc();
            ASSERT( !cursor->getsetdup( cursor->currLoc() ) );
            cursor->advance();

            // This is the 6,8 document, first time seen.  Not a dup.
            DiskLoc secondLoc = cursor->currLoc();
            ASSERT( !cursor->getsetdup( cursor->currLoc() ) );
            cursor->advance();

            // This is the 5,7 document, second time seen.  A dup.
            ASSERT_EQUALS( firstLoc, cursor->currLoc() );
            ASSERT( cursor->getsetdup( cursor->currLoc() ) );
            cursor->advance();

            // This is the 6,8 document, second time seen.  A dup.
            ASSERT_EQUALS( secondLoc, cursor->currLoc() );
            ASSERT( cursor->getsetdup( cursor->currLoc() ) );            
        }
    };

    /** Check that expected results are returned with inclusive bounds. */
    class InclusiveBounds {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );

            // Save 'a' values 1-10.
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );

            // Iterate over 'a' values 3-7 inclusive.
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 3 ),
                                                                  true,
                                                                  BSON( "" << 7 ),
                                                                  true ) );

            // Check that the expected 'a' values are returned.
            for( int32_t i = 3; i < 8; ++i, cursor->advance() ) {
                ASSERT_EQUALS( i, cursor->current()[ "a" ].Int() );
            }
            ASSERT( !cursor->ok() );
        }
    };

    /** Check that expected results are returned with exclusive bounds. */
    class ExclusiveBounds {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );

            // Save 'a' values 1-10.
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );

            // Iterate over 'a' values 3-7 exclusive.
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 3 ),
                                                                  false,
                                                                  BSON( "" << 7 ),
                                                                  false ) );

            // Check that the expected 'a' values are returned.
            for( int32_t i = 4; i < 7; ++i, cursor->advance() ) {
                ASSERT_EQUALS( i, cursor->current()[ "a" ].Int() );
            }
            ASSERT( !cursor->ok() );
        }
    };

    /** Check that killOp will interrupt cursor iteration. */
    class Interrupt {
    public:
        ~Interrupt() {
            // Reset curop's kill flag.
            cc().curop()->reset();
        }
        void run() {
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 150; ++i ) {
                _client.insert( _ns, BSON( "a" << 5 ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            Client::ReadContext ctx( _ns );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 5 ),
                                                                  true,
                                                                  BSON( "" << 5 ),
                                                                  true ) );

            // Register a request to kill the current operation.
            cc().curop()->kill();

            // Check that an exception is thrown when iterating the cursor.
            ASSERT_THROWS( exhaustCursor( cursor.get() ), UserException );
        }
    private:
        void exhaustCursor( Cursor* cursor ) {
            while( cursor->advance() );
        }
    };

    /** Check that a cursor returns no results if all documents are below the lower bound. */
    class NothingAboveLowerBound {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            _client.insert( _ns, BSON( "a" << 2 ) );
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 2 ),
                                                                  false,
                                                                  BSON( "" << 3 ),
                                                                  false ) );

            // The cursor returns no results.
            ASSERT( !cursor->ok() );
        }
    };

    /** Check that a cursor returns no results if there are no documents within the interval. */
    class NothingInInterval {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            _client.insert( _ns, BSON( "a" << 2 ) );
            _client.insert( _ns, BSON( "a" << 3 ) );
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 2 ),
                                                                  false,
                                                                  BSON( "" << 3 ),
                                                                  false ) );

            // The cursor returns no results.
            ASSERT( !cursor->ok() );
        }
    };
    
    /**
     * Check that a cursor returns no results if there are no documents within the interval and
     * the first key located during initialization is above the upper bound.
     */
    class NothingInIntervalFirstMatchBeyondUpperBound {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            _client.insert( _ns, BSON( "a" << 2 ) );
            _client.insert( _ns, BSON( "a" << 4 ) );
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );

            // Iterate over 'a' values ( 2, 3 ].
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                 nsdetails( _ns )->idx( 1 ),
                                                                 BSON( "" << 2 ),
                                                                 false,
                                                                 BSON( "" << 3 ),
                                                                 true ) );
            ASSERT( !cursor->ok() );
        }
    };

    /** Check that a cursor recovers its position properly if there is no change during a yield. */
    class NoChangeDuringYield {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 2 ),
                                                                  false,
                                                                  BSON( "" << 6 ),
                                                                  true ) );
            while( cursor->current()[ "a" ].Int() != 5 ) {
                cursor->advance();
            }

            // Prepare the cursor to yield.
            cursor->prepareToYield();

            // Recover the cursor.
            cursor->recoverFromYield();

            // The cursor is still at its original position.
            ASSERT_EQUALS( 5, cursor->current()[ "a" ].Int() );

            // The cursor can be advanced from this position.
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 6, cursor->current()[ "a" ].Int() );
        }
    };
    
    /**
     * Check that a cursor recovers its position properly if its current location is deleted
     * during a yield.
     */
    class DeleteDuringYield {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 2 ),
                                                                  false,
                                                                  BSON( "" << 6 ),
                                                                  true ) );
            while( cursor->current()[ "a" ].Int() != 5 ) {
                cursor->advance();
            }

            // Prepare the cursor to yield.
            cursor->prepareToYield();

            // Remove the current iterate and all remaining iterates.
            _client.remove( _ns, BSON( "a" << GTE << 5 ) );

            // Recover the cursor.
            cursor->recoverFromYield();

            // The cursor is exhausted.
            ASSERT( !cursor->ok() );
        }
    };
    
    /**
     * Check that a cursor relocates its end location properly if the end location changes during a
     * yield.
     */
    class InsertNewDocsDuringYield {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 2 ),
                                                                  false,
                                                                  BSON( "" << 6 ),
                                                                  true ) );
            while( cursor->current()[ "a" ].Int() != 4 ) {
                cursor->advance();
            }

            // Prepare the cursor to yield.
            cursor->prepareToYield();

            // Insert one doc before the end.
            _client.insert( _ns, BSON( "a" << 5.5 ) );

            // Insert one doc after the end.
            _client.insert( _ns, BSON( "a" << 6.5 ) );

            // Recover the cursor.
            cursor->recoverFromYield();

            // Check that the cursor returns the expected remaining documents.
            ASSERT_EQUALS( 4, cursor->current()[ "a" ].Int() );
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 5, cursor->current()[ "a" ].Int() );
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 5.5, cursor->current()[ "a" ].number() );
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 6, cursor->current()[ "a" ].Int() );
            ASSERT( !cursor->advance() );
        }
    };

    /** Check that isMultiKey() is updated correctly if an index becomes multikey during a yield. */
    class BecomesMultikeyDuringYield {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 2 ),
                                                                  false,
                                                                  BSON( "" << 50 ),
                                                                  true ) );
            while( cursor->current()[ "a" ].Int() != 4 ) {
                cursor->advance();
            }

            // Check that the cursor is not multikey.
            ASSERT( !cursor->isMultiKey() );

            // Prepare the cursor to yield.
            cursor->prepareToYield();

            // Insert a document with two values of 'a'.
            _client.insert( _ns, BSON( "a" << BSON_ARRAY( 10 << 11 ) ) );

            // Recover the cursor.
            cursor->recoverFromYield();

            // Check that the cursor becomes multikey.
            ASSERT( cursor->isMultiKey() );

            // Check that keys 10 and 11 are detected as duplicates.
            while( cursor->currKey().firstElement().Int() != 10 ) {
                ASSERT( cursor->advance() );
            }
            ASSERT( !cursor->getsetdup( cursor->currLoc() ) );
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 11, cursor->currKey().firstElement().Int() );
            ASSERT( cursor->getsetdup( cursor->currLoc() ) );
        }
    };

    /** Unused keys are not returned during iteration. */
    class UnusedKeys {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );

            // Mark keys at position 0, 3, and 4 as unused.
            nsdetails( _ns )->idx( 1 ).head.btreemod<V1>()->_k( 0 ).setUnused();
            nsdetails( _ns )->idx( 1 ).head.btreemod<V1>()->_k( 3 ).setUnused();
            nsdetails( _ns )->idx( 1 ).head.btreemod<V1>()->_k( 4 ).setUnused();

            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 0 ),
                                                                  true,
                                                                  BSON( "" << 6 ),
                                                                  true ) );
            
            // Check that the unused keys are not returned but the other keys are returned.
            ASSERT_EQUALS( 1, cursor->current()[ "a" ].Int() );
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 2, cursor->current()[ "a" ].Int() );
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 5, cursor->current()[ "a" ].Int() );
            ASSERT( cursor->advance() );
            ASSERT_EQUALS( 6, cursor->current()[ "a" ].Int() );
            ASSERT( !cursor->advance() );
        }
    };

    /** Iteration is properly terminated when the end location is an unused key. */
    class UnusedEndKey {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );

            // Mark the key at position 5 as unused.
            nsdetails( _ns )->idx( 1 ).head.btreemod<V1>()->_k( 5 ).setUnused();

            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                  nsdetails( _ns )->idx( 1 ),
                                                                  BSON( "" << 4 ),
                                                                  true,
                                                                  BSON( "" << 5 ),
                                                                  false ) );
            
            // Check the documents produced by the cursor.
            ASSERT_EQUALS( 4, cursor->current()[ "a" ].Int() );
            ASSERT( !cursor->advance() );
        }
    };

    /** Advances past a key that becomes unused during a yield. */
    class KeyBecomesUnusedDuringYield {
    public:
        void run() {
            Client::WriteContext ctx( _ns );
            _client.dropCollection( _ns );
            for( int32_t i = 0; i < 10; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            
            scoped_ptr<Cursor> cursor( IntervalBtreeCursor::make( nsdetails( _ns ),
                                                                 nsdetails( _ns )->idx( 1 ),
                                                                 BSON( "" << 3 ),
                                                                 true,
                                                                 BSON( "" << 9 ),
                                                                 true ) );
            
            // Advance the cursor to key a:5.
            while( cursor->current()[ "a" ].Int() != 5 ) {
                cursor->advance();
            }

            // Backup the cursor position.
            cursor->prepareToYield();

            // Mark the key at position 5 as unused.
            nsdetails( _ns )->idx( 1 ).head.btreemod<V1>()->_k( 5 ).setUnused();

            // Restore the cursor position.
            cursor->recoverFromYield();

            // The cursor advanced from 5, now unused, to 6.
            ASSERT_EQUALS( 6, cursor->current()[ "a" ].Int() );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "intervalbtreecursor" ) {
        }
        void setupTests() {
            add<WrongIndexVersion>();
            add<BasicAccessors>();
            add<Nscanned>();
            add<Matcher>();
            add<Dups>();
            add<InclusiveBounds>();
            add<ExclusiveBounds>();
            add<Interrupt>();
            add<NothingAboveLowerBound>();
            add<NothingInInterval>();
            add<NothingInIntervalFirstMatchBeyondUpperBound>();
            add<NoChangeDuringYield>();
            add<DeleteDuringYield>();
            add<InsertNewDocsDuringYield>();
            add<BecomesMultikeyDuringYield>();
            add<UnusedKeys>();
            add<UnusedEndKey>();
            add<KeyBecomesUnusedDuringYield>();
        }
    } myall;

} // namespace IntervalBtreeCursorTests
