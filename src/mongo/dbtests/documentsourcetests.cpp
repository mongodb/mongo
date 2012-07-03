// documentsourcetests.cpp : Unit tests for DocumentSource classes.

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

#include "pch.h"
#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/interrupt_status_mongod.h"
#include "mongo/db/pipeline/expression_context.h"

#include "dbtests.h"

namespace DocumentSourceTests {

    static const char* const ns = "unittests.documentsourcetests";
    static DBDirectClient client;

    class CollectionBase {
    public:
        ~CollectionBase() {
            client.dropCollection( ns );
        }
    };

    namespace DocumentSourceCursor {

        using mongo::DocumentSourceCursor;

        class Base : public CollectionBase {
        public:
            Base() :
                CollectionBase(),
                _ctx( ExpressionContext::create( &InterruptStatusMongod::status ) ) {
            }
        protected:
            void createSource() {
                shared_ptr<DocumentSourceCursor::CursorWithContext> cursorWithContext
                        ( new DocumentSourceCursor::CursorWithContext( ns ) );
                shared_ptr<Cursor> cursor = theDataFileMgr.findAll( ns );
                cursorWithContext->_cursor.reset
                        ( new ClientCursor( QueryOption_NoCursorTimeout, cursor, ns ) );
                _source = DocumentSourceCursor::create( cursorWithContext, _ctx );
            }
            intrusive_ptr<ExpressionContext> ctx() { return _ctx; }
            DocumentSourceCursor* source() { return _source.get(); }
        private:
            intrusive_ptr<ExpressionContext> _ctx;
            intrusive_ptr<DocumentSourceCursor> _source;
        };

        /** Create a DocumentSourceCursor. */
        class Create : public Base {
        public:
            void run() {
                createSource();
                // The CursorWithContext creates a read lock.
                ASSERT( Lock::isReadLocked() );
                // The CursorWithContext holds a ClientCursor.
                assertNumClientCursors( 1 );
                // The collection is empty, so the source produces no results.
                ASSERT( source()->eof() );
                // Exhausting the source releases the read lock.
                ASSERT( !Lock::isReadLocked() );
                // The ClientCursor is also cleaned up.
                assertNumClientCursors( 0 );
            }
        private:
            void assertNumClientCursors( unsigned int expected ) {
                set<CursorId> nsCursors;
                ClientCursor::find( ns, nsCursors );
                ASSERT_EQUALS( expected, nsCursors.size() );
            }
        };

        /** Iterate a DocumentSourceCursor. */
        class Iterate : public Base {
        public:
            void run() {
                client.insert( ns, BSON( "a" << 1 ) );
                createSource();
                // The CursorWithContext creates a read lock.
                ASSERT( Lock::isReadLocked() );
                // The cursor will produce a result.
                ASSERT( !source()->eof() );
                // The read lock is stil held.
                ASSERT( Lock::isReadLocked() );
                // The result is as expected.
                ASSERT_EQUALS( 1, source()->getCurrent()->getValue( "a" )->coerceToInt() );
                // There are no more results.
                ASSERT( !source()->advance() );
                // Exhausting the source releases the read lock.
                ASSERT( !Lock::isReadLocked() );                
            }
        };

        /** Dispose of a DocumentSourceCursor. */
        class Dispose : public Base {
        public:
            void run() {
                createSource();
                // The CursorWithContext creates a read lock.
                ASSERT( Lock::isReadLocked() );
                source()->dispose();
                // Releasing the cursor releases the read lock.
                ASSERT( !Lock::isReadLocked() );
                // The source is marked as exhausted.
                ASSERT( source()->eof() );
            }
        };
        
        /** Iterate a DocumentSourceCursor and then dispose of it. */
        class IterateDispose : public Base {
        public:
            void run() {
                client.insert( ns, BSON( "a" << 1 ) );
                client.insert( ns, BSON( "a" << 2 ) );
                client.insert( ns, BSON( "a" << 3 ) );
                createSource();
                ASSERT( !source()->eof() );
                // The result is as expected.
                ASSERT_EQUALS( 1, source()->getCurrent()->getValue( "a" )->coerceToInt() );
                // Get the next result.
                ASSERT( source()->advance() );
                // The result is as expected.
                ASSERT_EQUALS( 2, source()->getCurrent()->getValue( "a" )->coerceToInt() );
                // The source still holds the lock.
                ASSERT( Lock::isReadLocked() );
                source()->dispose();
                // Disposing of the source releases the lock.
                ASSERT( !Lock::isReadLocked() );
                // The source cannot be advanced further.
                ASSERT( !source()->advance() );
                ASSERT( source()->eof() );
                ASSERT( !source()->getCurrent() );
            }
        };

    } // namespace DocumentSourceCursor

    namespace DocumentSourceLimit {

        using mongo::DocumentSourceLimit;

        class Base : public DocumentSourceCursor::Base {
        protected:
            void createLimit( int limit ) {
                BSONObj spec = BSON( "$limit" << limit );
                BSONElement specElement = spec.firstElement();
                _limit = DocumentSourceLimit::createFromBson( &specElement, ctx() );
            }
            DocumentSource* limit() { return _limit.get(); }
        private:
            intrusive_ptr<DocumentSource> _limit;
        };

        /** Exhausting a DocumentSourceLimit disposes of the limit's source. */
        class DisposeSource : public Base {
        public:
            void run() {
                client.insert( ns, BSON( "a" << 1 ) );
                client.insert( ns, BSON( "a" << 2 ) );
                createSource();
                // The source holds a read lock.
                ASSERT( Lock::isReadLocked() );
                createLimit( 1 );
                limit()->setSource( source() );
                // The limit is not exhauted.
                ASSERT( !limit()->eof() );
                // The limit's result is as expected.
                ASSERT_EQUALS( 1, limit()->getCurrent()->getValue( "a" )->coerceToInt() );
                // The limit is exhausted.
                ASSERT( !limit()->advance() );
                // The limit disposes the source, releasing the read lock.
                ASSERT( !Lock::isReadLocked() );
            }
        };

        /** Exhausting a DocumentSourceLimit disposes of the pipeline's DocumentSourceCursor. */
        class DisposeSourceCascade : public Base {
        public:
            void run() {
                client.insert( ns, BSON( "a" << 1 ) );
                client.insert( ns, BSON( "a" << 1 ) );
                createSource();

                // Create a DocumentSourceMatch.
                BSONObj spec = BSON( "$match" << BSON( "a" << 1 ) );
                BSONElement specElement = spec.firstElement();
                intrusive_ptr<DocumentSource> match =
                        DocumentSourceMatch::createFromBson( &specElement, ctx() );
                match->setSource( source() );

                createLimit( 1 );
                limit()->setSource( match.get() );
                // The limit is not exhauted.
                ASSERT( !limit()->eof() );
                // The limit's result is as expected.
                ASSERT_EQUALS( 1, limit()->getCurrent()->getValue( "a" )->coerceToInt() );
                // The limit is exhausted.
                ASSERT( !limit()->advance() );
                // The limit disposes the match, which disposes the source and releases the read
                // lock.
                ASSERT( !Lock::isReadLocked() );
            }
        };

    } // namespace DocumentSourceLimit

    class All : public Suite {
    public:
        All() : Suite( "documentsource" ) {
        }
        void setupTests() {
            add<DocumentSourceCursor::Create>();
            add<DocumentSourceCursor::Iterate>();
            add<DocumentSourceCursor::Dispose>();
            add<DocumentSourceCursor::IterateDispose>();
            add<DocumentSourceLimit::DisposeSource>();
            add<DocumentSourceLimit::DisposeSourceCascade>();
        }
    } myall;

} // namespace DocumentSourceTests
