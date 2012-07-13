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

#include <boost/thread/thread.hpp>

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
                boost::shared_ptr<DocumentSourceCursor::CursorWithContext> cursorWithContext
                        ( new DocumentSourceCursor::CursorWithContext( ns ) );
                boost::shared_ptr<Cursor> cursor = theDataFileMgr.findAll( ns );
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

        /** Set a value or await an expected value. */
        class PendingValue {
        public:
            PendingValue( int initialValue ) :
            _value( initialValue ),
            _mutex( "DocumentSourceTests::PendingValue::_mutex" ) {
            }
            void set( int newValue ) {
                scoped_lock lk( _mutex );
                _value = newValue;
                _condition.notify_all();
            }
            void await( int expectedValue ) const {
                scoped_lock lk( _mutex );
                while( _value != expectedValue ) {
                    _condition.wait( lk.boost() );
                }
            }
        private:
            int _value;
            mutable mongo::mutex _mutex;
            mutable boost::condition _condition;
        };

        /** A writer client will be registered for the lifetime of an object of this class. */
        class WriterClientScope {
        public:
            WriterClientScope() :
            _state( Initial ),
            _dummyWriter( boost::bind( &WriterClientScope::runDummyWriter, this ) ) {
                _state.await( Ready );
            }
            ~WriterClientScope() {
                // Terminate the writer thread even on exception.
                _state.set( Finished );
                DESTRUCTOR_GUARD( _dummyWriter.join() );
            }
        private:
            enum State {
                Initial,
                Ready,
                Finished
            };
            void runDummyWriter() {
                Client::initThread( "dummy writer" );
                scoped_ptr<Acquiring> a( new Acquiring( 0 , cc().lockState() ) );
                _state.set( Ready );
                _state.await( Finished );
                a.reset(0);
                cc().shutdown();
            }
            PendingValue _state;
            boost::thread _dummyWriter;
        };

        /** DocumentSourceCursor yields deterministically when enough documents are scanned. */
        class Yield : public Base {
        public:
            void run() {
                // Insert enough documents that counting them will exceed the iteration threshold
                // to trigger a yield.
                for( int i = 0; i < 1000; ++i ) {
                    client.insert( ns, BSON( "a" << 1 ) );
                }
                createSource();
                ASSERT_EQUALS( 0, cc().curop()->numYields() );
                // Iterate through all results.
                while( source()->advance() );
                // The lock was yielded during iteration.
                ASSERT( 0 < cc().curop()->numYields() );
            }
        private:
            // An active writer is required to trigger yielding.
            WriterClientScope _writerScope;
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

    namespace DocumentSourceUnwind {

        using mongo::DocumentSourceUnwind;

        class Base : public DocumentSourceCursor::Base {
        protected:
            void createUnwind( const string& unwindFieldPath = "$a" ) {
                BSONObj spec = BSON( "$unwind" << unwindFieldPath );
                BSONElement specElement = spec.firstElement();
                _unwind = DocumentSourceUnwind::createFromBson( &specElement, ctx() );
                checkBsonRepresentation( spec );
                _unwind->setSource( source() );
            }
            DocumentSource* unwind() { return _unwind.get(); }
            /** Assert that iterator state accessors consistently report the source is exhausted. */
            void assertExhausted() const {
                // eof() is true.
                ASSERT( _unwind->eof() );
                // advance() does not assert, and returns false.
                ASSERT( !_unwind->advance() );
                // getCurrent() does not assert, and returns an empty pointer.
                ASSERT( !_unwind->getCurrent() );
            }
        private:
            /**
             * Check that the BSON representation generated by the souce matches the BSON it was
             * created with.
             */
            void checkBsonRepresentation( const BSONObj& spec ) {
                BSONArrayBuilder bab;
                _unwind->addToBsonArray( &bab, false );
                BSONObj generatedSpec = bab.arr()[ 0 ].Obj().getOwned();
                ASSERT_EQUALS( spec, generatedSpec );
            }
            intrusive_ptr<DocumentSource> _unwind;
        };

        /** eof() is the first member function called. */
        class EofInit : public Base {
        public:
            void run() {
                client.insert( ns, BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 ) ) );
                createSource();
                createUnwind();
                // A result is available, so not eof().
                ASSERT( !unwind()->eof() );
            }
        };

        /** advance() is the first member function called. */
        class AdvanceInit : public Base {
        public:
            void run() {
                client.insert( ns, BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 << 2 ) ) );
                createSource();
                createUnwind();
                // Another result is available, so advance() succeeds.
                ASSERT( unwind()->advance() );
                ASSERT_EQUALS( 2, unwind()->getCurrent()->getField( "a" )->coerceToInt() );
            }
        };

        /** getCurrent() is the first member function called. */
        class GetCurrentInit : public Base {
        public:
            void run() {
                client.insert( ns, BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 ) ) );
                createSource();
                createUnwind();
                // The first result exists and is as expected.
                ASSERT( unwind()->getCurrent() );
                ASSERT_EQUALS( 1, unwind()->getCurrent()->getField( "a" )->coerceToInt() );
            }
        };

        class CheckResultsBase : public Base {
        public:
            virtual ~CheckResultsBase() {}
            void run() {
                populateData();
                createSource();
                createUnwind( unwindFieldPath() );

                // Load the results from the DocumentSourceUnwind.
                vector<intrusive_ptr<Document> > resultSet;
                while( !unwind()->eof() ) {

                    // If not eof, current is non null.
                    ASSERT( unwind()->getCurrent() );

                    // Get the current result.
                    resultSet.push_back( unwind()->getCurrent() );

                    // Advance.
                    if ( unwind()->advance() ) {
                        // If advance succeeded, eof() is false.
                        ASSERT( !unwind()->eof() );
                    }
                }
                // Verify the DocumentSourceUnwind is exhausted.
                assertExhausted();

                // Convert results to BSON once they all have been retrieved (to detect any errors
                // resulting from incorrectly shared sub objects).
                BSONArrayBuilder bsonResultSet;
                for( vector<intrusive_ptr<Document> >::const_iterator i = resultSet.begin();
                     i != resultSet.end(); ++i ) {
                    BSONObjBuilder bob;
                    (*i)->toBson( &bob );
                    bsonResultSet << bob.obj();
                }
                // Check the result set.
                ASSERT_EQUALS( expectedResultSet(), bsonResultSet.arr() );
            }
        protected:
            virtual void populateData() {}
            virtual BSONObj expectedResultSet() const {
                BSONObj wrappedResult =
                        // fromjson cannot parse an array, so place the array within an object.
                        fromjson( string( "{'':" ) + expectedResultSetString() + "}" );
                return wrappedResult[ "" ].embeddedObject().getOwned();
            }
            virtual string expectedResultSetString() const { return "[]"; }
            virtual string unwindFieldPath() const { return "$a"; }
        };

        class UnexpectedTypeBase : public Base {
        public:
            virtual ~UnexpectedTypeBase() {}
            void run() {
                populateData();
                createSource();
                createUnwind();
                // A UserException is thrown during iteration.
                ASSERT_THROWS( iterateAll(), UserException );
            }
        protected:
            virtual void populateData() {}
        private:
            void iterateAll() {
                while( unwind()->advance() );
            }
        };

        /** An empty collection produces no results. */
        class Empty : public CheckResultsBase {
        };

        /** A document without the unwind field produces no results. */
        class MissingField : public CheckResultsBase {
            void populateData() {
                client.insert( ns, BSONObj() );
            }
        };

        /** A document with a null field produces no results. */
        class NullField : public CheckResultsBase {
            void populateData() {
                client.insert( ns, BSON( "a" << BSONNULL ) );
            }
        };

        /** A document with an empty array produces no results. */
        class EmptyArray : public CheckResultsBase {
            void populateData() {
                client.insert( ns, BSON( "a" << BSONArray() ) );
            }
        };

        /** A document with a number field produces a UserException. */
        class UnexpectedNumber : public UnexpectedTypeBase {
            void populateData() {
                client.insert( ns, BSON( "a" << 1 ) );
            }
        };

        /** An additional document with a number field produces a UserException. */
        class LaterUnexpectedNumber : public UnexpectedTypeBase {
            void populateData() {
                client.insert( ns, BSON( "a" << BSON_ARRAY( 1 ) ) );
                client.insert( ns, BSON( "a" << 1 ) );
            }
        };

        /** A document with a string field produces a UserException. */
        class UnexpectedString : public UnexpectedTypeBase {
            void populateData() {
                client.insert( ns, BSON( "a" << "foo" ) );
            }
        };

        /** A document with an object field produces a UserException. */
        class UnexpectedObject : public UnexpectedTypeBase {
            void populateData() {
                client.insert( ns, BSON( "a" << BSONObj() ) );
            }
        };

        /** Unwind an array with one value. */
        class UnwindOneValue : public CheckResultsBase {
            void populateData() {
                client.insert( ns, BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 ) ) );
            }
            string expectedResultSetString() const { return "[{_id:0,a:1}]"; }
        };

        /** Unwind an array with two values. */
        class UnwindTwoValues : public CheckResultsBase {
            void populateData() {
                client.insert( ns, BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 << 2 ) ) );
            }
            string expectedResultSetString() const { return "[{_id:0,a:1},{_id:0,a:2}]"; }
        };

        /** Unwind an array with two values, one of which is null. */
        class UnwindNull : public CheckResultsBase {
            void populateData() {
                client.insert( ns, fromjson( "{_id:0,a:[1,null]}" ) );
            }
            string expectedResultSetString() const { return "[{_id:0,a:1},{_id:0,a:null}]"; }
        };

        /** Unwind two documents with arrays. */
        class TwoDocuments : public CheckResultsBase {
            void populateData() {
                client.insert( ns, fromjson( "{_id:0,a:[1,2]}" ) );
                client.insert( ns, fromjson( "{_id:1,a:[3,4]}" ) );
            }
            string expectedResultSetString() const {
                return "[{_id:0,a:1},{_id:0,a:2},{_id:1,a:3},{_id:1,a:4}]";
            }
        };

        /** Unwind an array in a nested document. */
        class NestedArray : public CheckResultsBase {
            void populateData() {
                client.insert( ns, fromjson( "{_id:0,a:{b:[1,2],c:3}}" ) );
            }
            string expectedResultSetString() const {
                return "[{_id:0,a:{b:1,c:3}},{_id:0,a:{b:2,c:3}}]";
            }
            string unwindFieldPath() const { return "$a.b"; }
        };

        /** A missing array (that cannot be nested below a non object field) produces no results. */
        class NonObjectParent : public CheckResultsBase {
            void populateData() {
                client.insert( ns, fromjson( "{_id:0,a:4}" ) );
            }
            string unwindFieldPath() const { return "$a.b"; }
        };

        /** Unwind an array in a doubly nested document. */
        class DoubleNestedArray : public CheckResultsBase {
            void populateData() {
                client.insert( ns, fromjson( "{_id:0,a:{b:{d:[1,2],e:4},c:3}}" ) );
            }
            string expectedResultSetString() const {
                return "[{_id:0,a:{b:{d:1,e:4},c:3}},{_id:0,a:{b:{d:2,e:4},c:3}}]";
            }
            string unwindFieldPath() const { return "$a.b.d"; }
        };

        /** Unwind several documents in a row. */
        class SeveralDocuments : public CheckResultsBase {
            void populateData() {
                client.insert( ns, fromjson( "{_id:0,a:[1,2,3]}" ) );
                client.insert( ns, fromjson( "{_id:1}" ) );
                client.insert( ns, fromjson( "{_id:2}" ) );
                client.insert( ns, fromjson( "{_id:3,a:[10,20]}" ) );
                client.insert( ns, fromjson( "{_id:4,a:[30]}" ) );
            }
            string expectedResultSetString() const {
                return "[{_id:0,a:1},{_id:0,a:2},{_id:0,a:3},{_id:3,a:10},"
                        "{_id:3,a:20},{_id:4,a:30}]";
            }
        };

        /** Unwind several more documents in a row. */
        class SeveralMoreDocuments : public CheckResultsBase {
            void populateData() {
                client.insert( ns, fromjson( "{_id:0,a:null}" ) );
                client.insert( ns, fromjson( "{_id:1}" ) );
                client.insert( ns, fromjson( "{_id:2,a:['a','b']}" ) );
                client.insert( ns, fromjson( "{_id:3}" ) );
                client.insert( ns, fromjson( "{_id:4,a:[1,2,3]}" ) );
                client.insert( ns, fromjson( "{_id:5,a:[4,5,6]}" ) );
                client.insert( ns, fromjson( "{_id:6,a:[7,8,9]}" ) );
                client.insert( ns, fromjson( "{_id:7,a:[]}" ) );
            }
            string expectedResultSetString() const {
                return "[{_id:2,a:'a'},{_id:2,a:'b'},{_id:4,a:1},{_id:4,a:2},"
                        "{_id:4,a:3},{_id:5,a:4},{_id:5,a:5},{_id:5,a:6},"
                        "{_id:6,a:7},{_id:6,a:8},{_id:6,a:9}]";
            }
        };

    } // namespace DocumentSourceUnwind

    class All : public Suite {
    public:
        All() : Suite( "documentsource" ) {
        }
        void setupTests() {
            add<DocumentSourceCursor::Create>();
            add<DocumentSourceCursor::Iterate>();
            add<DocumentSourceCursor::Dispose>();
            add<DocumentSourceCursor::IterateDispose>();
            add<DocumentSourceCursor::Yield>();
            add<DocumentSourceLimit::DisposeSource>();
            add<DocumentSourceLimit::DisposeSourceCascade>();
            add<DocumentSourceUnwind::EofInit>();
            add<DocumentSourceUnwind::AdvanceInit>();
            add<DocumentSourceUnwind::GetCurrentInit>();
            add<DocumentSourceUnwind::Empty>();
            add<DocumentSourceUnwind::MissingField>();
            add<DocumentSourceUnwind::NullField>();
            add<DocumentSourceUnwind::EmptyArray>();
            add<DocumentSourceUnwind::UnexpectedNumber>();
            add<DocumentSourceUnwind::LaterUnexpectedNumber>();
            add<DocumentSourceUnwind::UnexpectedString>();
            add<DocumentSourceUnwind::UnexpectedObject>();
            add<DocumentSourceUnwind::UnwindOneValue>();
            add<DocumentSourceUnwind::UnwindTwoValues>();
            add<DocumentSourceUnwind::UnwindNull>();
            add<DocumentSourceUnwind::TwoDocuments>();
            add<DocumentSourceUnwind::NestedArray>();
            add<DocumentSourceUnwind::NonObjectParent>();
            add<DocumentSourceUnwind::DoubleNestedArray>();
            add<DocumentSourceUnwind::SeveralDocuments>();
            add<DocumentSourceUnwind::SeveralMoreDocuments>();
        }
    } myall;

} // namespace DocumentSourceTests
