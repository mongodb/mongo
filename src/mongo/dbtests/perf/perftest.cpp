// perftest.cpp : Run db performance tests.
//

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "mongo/pch.h"

#include <boost/date_time/posix_time/posix_time.hpp>

#include "mongo/base/initializer.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/query_optimizer_internal.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/dbtests/framework.h"
#include "mongo/util/file_allocator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    extern string dbpath;

    // This specifies default dbpath for our testing framework
    const std::string default_test_dbpath = "/data/db/perftest";
} // namespace mongo


using namespace mongo;
using namespace mongo::unittest;

DBClientBase *client_;

// Each test runs with a separate db, so no test does any of the startup
// (ie allocation) work for another test.
template< class T >
string testDb( T *t = 0 ) {
    string name = mongo::demangleName( typeid( T ) );
    // Make filesystem safe.
    for( string::iterator i = name.begin(); i != name.end(); ++i )
        if ( *i == ':' )
            *i = '_';
    return name;
}

template< class T >
string testNs( T *t ) {
    stringstream ss;
    ss << testDb( t ) << ".perftest";
    return ss.str();
}

template <class T>
class TestRunner {
public:
    void run() {
        T test;
        string name = testDb( &test );
        boost::posix_time::ptime start = boost::posix_time::microsec_clock::universal_time();
        test.run();
        boost::posix_time::ptime end = boost::posix_time::microsec_clock::universal_time();
        long long micro = ( end - start ).total_microseconds();
        cout << "{'" << name << "': "
             << micro / 1000000
             << "."
             << setw( 6 ) << setfill( '0' ) << micro % 1000000
             << "}" << endl;
    }
    ~TestRunner() {
        FileAllocator::get()->waitUntilFinished();
        client_->dropDatabase( testDb< T >().c_str() );
    }
};

class RunnerSuite : public Suite {
public:
    RunnerSuite( string name ) : Suite( name ) {}
protected:
    template< class T >
    void add() {
        Suite::add< TestRunner< T > >();
    }
};

namespace Insert {
    class IdIndex {
    public:
        void run() {
            string ns = testNs( this );
            for( int i = 0; i < 100000; ++i ) {
                client_->insert( ns.c_str(), BSON( "_id" << i ) );
            }
        }
    };

    class TwoIndex {
    public:
        TwoIndex() : ns_( testNs( this ) ) {
            client_->ensureIndex( ns_, BSON( "_id" << 1 ), "my_id" );
        }
        void run() {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i ) );
        }
        string ns_;
    };

    class TenIndex {
    public:
        TenIndex() : ns_( testNs( this ) ) {
            const char *names = "aaaaaaaaa";
            for( int i = 0; i < 9; ++i ) {
                client_->resetIndexCache();
                client_->ensureIndex( ns_.c_str(), BSON( "_id" << 1 ), false, names + i );
            }
        }
        void run() {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i ) );
        }
        string ns_;
    };

    class Capped {
    public:
        Capped() : ns_( testNs( this ) ) {
            client_->createCollection( ns_.c_str(), 100000, true );
        }
        void run() {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i ) );
        }
        string ns_;
    };

    class OneIndexReverse {
    public:
        OneIndexReverse() : ns_( testNs( this ) ) {
            client_->ensureIndex( ns_, BSON( "_id" << 1 ) );
        }
        void run() {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << ( 100000 - 1 - i ) ) );
        }
        string ns_;
    };

    class OneIndexHighLow {
    public:
        OneIndexHighLow() : ns_( testNs( this ) ) {
            client_->ensureIndex( ns_, BSON( "_id" << 1 ) );
        }
        void run() {
            for( int i = 0; i < 100000; ++i ) {
                int j = 50000 + ( ( i % 2 == 0 ) ? 1 : -1 ) * ( i / 2 + 1 );
                client_->insert( ns_.c_str(), BSON( "_id" << j ) );
            }
        }
        string ns_;
    };

    class All : public RunnerSuite {
    public:
        All() : RunnerSuite( "insert" ) {}

        void setupTests() {
            add< IdIndex >();
            add< TwoIndex >();
            add< TenIndex >();
            add< Capped >();
            add< OneIndexReverse >();
            add< OneIndexHighLow >();
        }
    } all;
} // namespace Insert

namespace Update {
    class Smaller {
    public:
        Smaller() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i << "b" << 2 ) );
        }
        void run() {
            for( int i = 0; i < 100000; ++i )
                client_->update( ns_.c_str(), QUERY( "_id" << i ), BSON( "_id" << i ) );
        }
        string ns_;
    };

    class Bigger {
    public:
        Bigger() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i ) );
        }
        void run() {
            for( int i = 0; i < 100000; ++i )
                client_->update( ns_.c_str(), QUERY( "_id" << i ), BSON( "_id" << i << "b" << 2 ) );
        }
        string ns_;
    };

    class Inc {
    public:
        Inc() : ns_( testNs( this ) ) {
            for( int i = 0; i < 10000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i << "i" << 0 ) );
        }
        void run() {
            for( int j = 0; j < 10; ++j )
                for( int i = 0; i < 10000; ++i )
                    client_->update( ns_.c_str(), QUERY( "_id" << i ), BSON( "$inc" << BSON( "i" << 1 ) ) );
        }
        string ns_;
    };

    class Set {
    public:
        Set() : ns_( testNs( this ) ) {
            for( int i = 0; i < 10000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i << "i" << 0 ) );
        }
        void run() {
            for( int j = 1; j < 11; ++j )
                for( int i = 0; i < 10000; ++i )
                    client_->update( ns_.c_str(), QUERY( "_id" << i ), BSON( "$set" << BSON( "i" << j ) ) );
        }
        string ns_;
    };

    class SetGrow {
    public:
        SetGrow() : ns_( testNs( this ) ) {
            for( int i = 0; i < 10000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i << "i" << "" ) );
        }
        void run() {
            for( int j = 9; j > -1; --j )
                for( int i = 0; i < 10000; ++i )
                    client_->update( ns_.c_str(), QUERY( "_id" << i ), BSON( "$set" << BSON( "i" << "aaaaaaaaaa"[j] ) ) );
        }
        string ns_;
    };

    class All : public RunnerSuite {
    public:
        All() : RunnerSuite( "update" ) {}
        void setupTests() {
            add< Smaller >();
            add< Bigger >();
            add< Inc >();
            add< Set >();
            add< SetGrow >();
        }
    } all;
} // namespace Update

namespace BSON {

    const char *sample =
        "{\"one\":2, \"two\":5, \"three\": {},"
        "\"four\": { \"five\": { \"six\" : 11 } },"
        "\"seven\": [ \"a\", \"bb\", \"ccc\", 5 ],"
        "\"eight\": Dbref( \"rrr\", \"01234567890123456789aaaa\" ),"
        "\"_id\": ObjectId( \"deadbeefdeadbeefdeadbeef\" ),"
        "\"nine\": { \"$binary\": \"abc=\", \"$type\": \"02\" },"
        "\"ten\": Date( 44 ), \"eleven\": /foooooo/i }";

    const char *shopwikiSample =
        "{ '_id' : '289780-80f85380b5c1d4a0ad75d1217673a4a2' , 'site_id' : 289780 , 'title'"
        ": 'Jubilee - Margaret Walker' , 'image_url' : 'http://www.heartlanddigsandfinds.c"
        "om/store/graphics/Product_Graphics/Product_8679.jpg' , 'url' : 'http://www.heartla"
        "nddigsandfinds.com/store/store_product_detail.cfm?Product_ID=8679&Category_ID=2&Su"
        "b_Category_ID=910' , 'url_hash' : 3450626119933116345 , 'last_update' :  null  , '"
        "features' : { '$imagePrefetchDate' : '2008Aug30 22:39' , '$image.color.rgb' : '5a7"
        "574' , 'Price' : '$10.99' , 'Description' : 'Author--s 1st Novel. A Houghton Miffl"
        "in Literary Fellowship Award novel by the esteemed poet and novelist who has demon"
        "strated a lifelong commitment to the heritage of black culture. An acclaimed story"
        "of Vyry, a negro slave during the 19th Century, facing the biggest challenge of h"
        "er lifetime - that of gaining her freedom, fighting for all the things she had nev"
        "er known before. The author, great-granddaughter of Vyry, reveals what the Civil W"
        "ar in America meant to the Negroes. Slavery W' , '$priceHistory-1' : '2008Dec03 $1"
        "0.99' , 'Brand' : 'Walker' , '$brands_in_title' : 'Walker' , '--path' : '//HTML[1]"
        "/BODY[1]/TABLE[1]/TR[1]/TD[1]/P[1]/TABLE[1]/TR[1]/TD[1]/TABLE[1]/TR[2]/TD[2]/TABLE"
        "[1]/TR[1]/TD[1]/P[1]/TABLE[1]/TR[1]' , '~location' : 'en_US' , '$crawled' : '2009J"
        "an11 03:22' , '$priceHistory-2' : '2008Nov15 $10.99' , '$priceHistory-0' : '2008De"
        "c24 $10.99'}}";

    class Parse {
    public:
        void run() {
            for( int i = 0; i < 10000; ++i )
                fromjson( sample );
        }
    };

    class ShopwikiParse {
    public:
        void run() {
            for( int i = 0; i < 10000; ++i )
                fromjson( shopwikiSample );
        }
    };

    class Json {
    public:
        Json() : o_( fromjson( sample ) ) {}
        void run() {
            for( int i = 0; i < 10000; ++i )
                o_.jsonString();
        }
        BSONObj o_;
    };

    class ShopwikiJson {
    public:
        ShopwikiJson() : o_( fromjson( shopwikiSample ) ) {}
        void run() {
            for( int i = 0; i < 10000; ++i )
                o_.jsonString();
        }
        BSONObj o_;
    };

    template <int LEN>
    class Copy {
    public:
        Copy(){
            // putting it in a subobject to force copy on getOwned
            BSONObjBuilder outer;
            BSONObjBuilder b (outer.subobjStart("inner"));
            while (b.len() < LEN)
                b.append(BSONObjBuilder::numStr(b.len()), b.len());
            b.done();
            _base = outer.obj();
        }

        void run() {
            int iterations = 1000*1000;
            while (iterations--){
                BSONObj temp = copy(_base.firstElement().embeddedObject().getOwned());
            }
        }

    private:
        // noinline should force copying even when optimized
        NOINLINE_DECL BSONObj copy(BSONObj x){
            return x;
        }

        BSONObj _base;
    };



    class All : public RunnerSuite {
    public:
        All() : RunnerSuite( "bson" ) {}
        void setupTests() {
            add< Parse >();
            add< ShopwikiParse >();
            add< Json >();
            add< ShopwikiJson >();
            add< Copy<10> >();
            add< Copy<100> >();
            add< Copy<1000> >();
            add< Copy<10*1000> >();
        }
    } all;

} // namespace BSON

namespace Index {

    class Int {
    public:
        Int() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "a" << i ) );
        }
        void run() {
            client_->ensureIndex( ns_, BSON( "a" << 1 ) );
        }
        string ns_;
    };

    class ObjectId {
    public:
        ObjectId() : ns_( testNs( this ) ) {
            OID id;
            for( int i = 0; i < 100000; ++i ) {
                id.init();
                client_->insert( ns_.c_str(), BSON( "a" << id ) );
            }
        }
        void run() {
            client_->ensureIndex( ns_, BSON( "a" << 1 ) );
        }
        string ns_;
    };

    class String {
    public:
        String() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i ) {
                stringstream ss;
                ss << i;
                client_->insert( ns_.c_str(), BSON( "a" << ss.str() ) );
            }
        }
        void run() {
            client_->ensureIndex( ns_, BSON( "a" << 1 ) );
        }
        string ns_;
    };

    class Object {
    public:
        Object() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i ) {
                client_->insert( ns_.c_str(), BSON( "a" << BSON( "a" << i ) ) );
            }
        }
        void run() {
            client_->ensureIndex( ns_, BSON( "a" << 1 ) );
        }
        string ns_;
    };

    class All : public RunnerSuite {
    public:
        All() : RunnerSuite( "index" ) {}
        void setupTests() {
            add< Int >();
            add< ObjectId >();
            add< String >();
            add< Object >();
        }
    } all;

} // namespace Index

namespace QueryTests {

    class NoMatch {
    public:
        NoMatch() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i ) );
        }
        void run() {
            client_->findOne( ns_.c_str(), QUERY( "_id" << 100000 ) );
        }
        string ns_;
    };

    class NoMatchIndex {
    public:
        NoMatchIndex() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i ) );
        }
        void run() {
            client_->findOne( ns_.c_str(),
                              QUERY( "a" << "b" ).hint( BSON( "_id" << 1 ) ) );
        }
        string ns_;
    };

    class NoMatchLong {
    public:
        NoMatchLong() : ns_( testNs( this ) ) {
            const char *names = "aaaaaaaaaa";
            for( int i = 0; i < 100000; ++i ) {
                BSONObjBuilder b;
                for( int j = 0; j < 10; ++j )
                    b << ( names + j ) << i;
                client_->insert( ns_.c_str(), b.obj() );
            }
        }
        void run() {
            client_->findOne( ns_.c_str(), QUERY( "a" << 100000 ) );
        }
        string ns_;
    };

    class SortOrdered {
    public:
        SortOrdered() : ns_( testNs( this ) ) {
            for( int i = 0; i < 50000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << i ) );
        }
        void run() {
            auto_ptr< DBClientCursor > c =
                client_->query( ns_.c_str(), Query( BSONObj() ).sort( BSON( "_id" << 1 ) ) );
            int i = 0;
            for( ; c->more(); c->nextSafe(), ++i );
            ASSERT_EQUALS( 50000, i );
        }
        string ns_;
    };

    class SortReverse {
    public:
        SortReverse() : ns_( testNs( this ) ) {
            for( int i = 0; i < 50000; ++i )
                client_->insert( ns_.c_str(), BSON( "_id" << ( 50000 - 1 - i ) ) );
        }
        void run() {
            auto_ptr< DBClientCursor > c =
                client_->query( ns_.c_str(), Query( BSONObj() ).sort( BSON( "_id" << 1 ) ) );
            int i = 0;
            for( ; c->more(); c->nextSafe(), ++i );
            ASSERT_EQUALS( 50000, i );
        }
        string ns_;
    };

    class GetMore {
    public:
        GetMore() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "a" << i ) );
            c_ = client_->query( ns_.c_str(), Query() );
        }
        void run() {
            int i = 0;
            for( ; c_->more(); c_->nextSafe(), ++i );
            ASSERT_EQUALS( 100000, i );
        }
        string ns_;
        auto_ptr< DBClientCursor > c_;
    };

    class GetMoreIndex {
    public:
        GetMoreIndex() : ns_( testNs( this ) ) {
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_.c_str(), BSON( "a" << i ) );
            client_->ensureIndex( ns_, BSON( "a" << 1 ) );
            c_ = client_->query( ns_.c_str(), QUERY( "a" << GT << -1 ).hint( BSON( "a" << 1 ) ) );
        }
        void run() {
            int i = 0;
            for( ; c_->more(); c_->nextSafe(), ++i );
            ASSERT_EQUALS( 100000, i );
        }
        string ns_;
        auto_ptr< DBClientCursor > c_;
    };

    class GetMoreKeyMatchHelps {
    public:
        GetMoreKeyMatchHelps() : ns_( testNs( this ) ) {
            for( int i = 0; i < 1000000; ++i )
                client_->insert( ns_.c_str(), BSON( "a" << i << "b" << i % 10 << "c" << "d" ) );
            client_->ensureIndex( ns_, BSON( "a" << 1 << "b" << 1 ) );
            c_ = client_->query( ns_.c_str(), QUERY( "a" << GT << -1 << "b" << 0 ).hint( BSON( "a" << 1 << "b" << 1 ) ) );
        }
        void run() {
            int i = 0;
            for( ; c_->more(); c_->nextSafe(), ++i );
            ASSERT_EQUALS( 100000, i );
        }
        string ns_;
        auto_ptr< DBClientCursor > c_;
    };

    class All : public RunnerSuite {
    public:
        All() : RunnerSuite( "query" ) {}
        void setupTests() {
            add< NoMatch >();
            add< NoMatchIndex >();
            add< NoMatchLong >();
            add< SortOrdered >();
            add< SortReverse >();
            add< GetMore >();
            add< GetMoreIndex >();
            add< GetMoreKeyMatchHelps >();
        }
    } all;

} // namespace QueryTests

namespace Count {

    class Count {
    public:
        Count() : ns_( testNs( this ) ) {
            BSONObj obj = BSON( "a" << 1 );
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_, obj );
        }
        void run() {
            ASSERT_EQUALS( 100000U, client_->count( ns_, BSON( "a" << 1 ) ) );
        }
        string ns_;
    };

    class CountIndex {
    public:
        CountIndex() : ns_( testNs( this ) ) {
            BSONObj obj = BSON( "a" << 1 );
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_, obj );
            client_->ensureIndex( ns_, obj );
        }
        void run() {
            // 'simple' match does not work for numbers
            ASSERT_EQUALS( 100000U, client_->count( ns_, BSON( "a" << 1 ) ) );
        }
        string ns_;
    };

    class CountSimpleIndex {
    public:
        CountSimpleIndex() : ns_( testNs( this ) ) {
            BSONObj obj = BSON( "a" << "b" );
            for( int i = 0; i < 100000; ++i )
                client_->insert( ns_, obj );
            client_->ensureIndex( ns_, obj );
        }
        void run() {
            ASSERT_EQUALS( 100000U, client_->count( ns_, BSON( "a" << "b" ) ) );
        }
        string ns_;
    };

    class All : public RunnerSuite {
    public:
        All() : RunnerSuite( "count" ) {}
        void setupTests() {
            add< Count >();
            add< CountIndex >();
            add< CountSimpleIndex >();
        }
    } all;

} // namespace Count

namespace Plan {

    class Hint {
    public:
        Hint() : ns_( testNs( this ) ) {
            const char *names = "aaaaaaaaa";
            for( int i = 0; i < 9; ++i ) {
                client_->resetIndexCache();
                client_->ensureIndex( ns_.c_str(), BSON( ( names + i ) << 1 ), false, names + i );
            }
            _lk.reset( new Lock::GlobalWrite );
            _ctx.reset( new Client::Context( ns_ ) );
            hint_ = BSON( "hint" << BSON( "a" << 1 ) );
        }
        void run() {
            for( int i = 0; i < 10000; ++i ) {
                scoped_ptr<MultiPlanScanner> s
                        ( MultiPlanScanner::make( ns_.c_str(), BSONObj(), BSONObj(),
                                                 boost::shared_ptr<const ParsedQuery>(), hint_ ) );
            }
        }
        string ns_;
        scoped_ptr<Lock::GlobalWrite> _lk;
        scoped_ptr<Client::Context> _ctx;
        BSONObj hint_;
    };

    class Sort {
    public:
        Sort() : ns_( testNs( this ) ) {
            const char *names = "aaaaaaaaaa";
            for( int i = 0; i < 10; ++i ) {
                client_->resetIndexCache();
                client_->ensureIndex( ns_.c_str(), BSON( ( names + i ) << 1 ), false, names + i );
            }
            lk_.reset( new Lock::GlobalWrite );
        }
        void run() {
            Client::Context ctx( ns_ );
            for( int i = 0; i < 10000; ++i ) {
                scoped_ptr<MultiPlanScanner> s
                        ( MultiPlanScanner::make( ns_.c_str(), BSONObj(), BSON( "a" << 1 ) ) );
            }
        }
        string ns_;
        auto_ptr< Lock::GlobalWrite > lk_;
    };

    class Query {
    public:
        Query() : ns_( testNs( this ) ) {
            const char *names = "aaaaaaaaaa";
            for( int i = 0; i < 10; ++i ) {
                client_->resetIndexCache();
                client_->ensureIndex( ns_.c_str(), BSON( ( names + i ) << 1 ), false, names + i );
            }
            lk_.reset( new Lock::GlobalWrite );
        }
        void run() {
            Client::Context ctx( ns_.c_str() );
            for( int i = 0; i < 10000; ++i ) {
                scoped_ptr<MultiPlanScanner>
                        s( MultiPlanScanner::make( ns_.c_str(), BSON( "a" << 1 ), BSONObj() ) );
            }
        }
        string ns_;
        auto_ptr< Lock::GlobalWrite > lk_;
    };

    class All : public RunnerSuite {
    public:
        All() : RunnerSuite("plan" ) {}
        void setupTests() {
            add< Hint >();
            add< Sort >();
            add< Query >();
        }
    } all;
} // namespace Plan

namespace Misc {
    class TimeMicros64 {
    public:
        void run() {
            int iterations = 1000*1000;
            while(iterations--){
                curTimeMicros64();
            }
        }
    };

    class JSTime {
    public:
        void run() {
            int iterations = 1000*1000;
            while(iterations--){
                jsTime();
            }
        }
    };

    class All : public RunnerSuite {
    public:
        All() : RunnerSuite("misc") {}
        void setupTests() {
            add< TimeMicros64 >();
            add< JSTime >();
        }
    } all;
}

int main( int argc, char **argv, char** envp ) {
    mongo::runGlobalInitializersOrDie(argc, argv, envp);

    mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(mongo::logger::LogSeverity::Log());
    client_ = new DBDirectClient();

    return mongo::dbtests::runDbTests(argc, argv);
}

