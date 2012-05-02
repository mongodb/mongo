// updatetests.cpp : unit tests relating to update requests
//

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

#include "pch.h"
#include "mongo/client/dbclientcursor.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"
#include "../db/ops/update.h"

#include "dbtests.h"

namespace UpdateTests {

    class ClientBase {
    public:
        // NOTE: Not bothering to backup the old error record.
        ClientBase() {
            mongo::lastError.reset( new LastError() );
        }
        ~ClientBase() {
            mongo::lastError.release();
        }
    protected:
        static void insert( const char *ns, BSONObj o ) {
            client_.insert( ns, o );
        }
        static void update( const char *ns, BSONObj q, BSONObj o, bool upsert = 0 ) {
            client_.update( ns, Query( q ), o, upsert );
        }
        static bool error() {
            return !client_.getPrevError().getField( "err" ).isNull();
        }
        DBDirectClient &client() const { return client_; }
    private:
        static DBDirectClient client_;
    };
    DBDirectClient ClientBase::client_;

    class Fail : public ClientBase {
    public:
        virtual ~Fail() {}
        void run() {
            prep();
            ASSERT( !error() );
            doIt();
            ASSERT( error() );
        }
    protected:
        const char *ns() { return "unittests.UpdateTests_Fail"; }
        virtual void prep() {
            insert( ns(), fromjson( "{a:1}" ) );
        }
        virtual void doIt() = 0;
    };

    class ModId : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$set:{'_id':4}}" ) );
        }
    };

    class ModNonmodMix : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$set:{a:4},z:3}" ) );
        }
    };

    class InvalidMod : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$awk:{a:4}}" ) );
        }
    };

    class ModNotFirst : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{z:3,$set:{a:4}}" ) );
        }
    };

    class ModDuplicateFieldSpec : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$set:{a:4},$inc:{a:1}}" ) );
        }
    };

    class IncNonNumber : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$inc:{a:'d'}}" ) );
        }
    };

    class PushAllNonArray : public Fail {
        void doIt() {
            insert( ns(), fromjson( "{a:[1]}" ) );
            update( ns(), BSONObj(), fromjson( "{$pushAll:{a:'d'}}" ) );
        }
    };

    class PullAllNonArray : public Fail {
        void doIt() {
            insert( ns(), fromjson( "{a:[1]}" ) );
            update( ns(), BSONObj(), fromjson( "{$pullAll:{a:'d'}}" ) );
        }
    };

    class IncTargetNonNumber : public Fail {
        void doIt() {
            insert( ns(), BSON( "a" << "a" ) );
            update( ns(), BSON( "a" << "a" ), fromjson( "{$inc:{a:1}}" ) );
        }
    };

    class SetBase : public ClientBase {
    public:
        ~SetBase() {
            client().dropCollection( ns() );
        }
    protected:
        const char *ns() { return "unittests.updatetests.SetBase"; }
    };

    class SetNum : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << 1 ) );
            client().update( ns(), BSON( "a" << 1 ), BSON( "$set" << BSON( "a" << 4 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << 4 ) ).isEmpty() );
        }
    };

    class SetString : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "b" ) );
            client().update( ns(), BSON( "a" << "b" ), BSON( "$set" << BSON( "a" << "c" ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << "c" ) ).isEmpty() );
        }
    };

    class SetStringDifferentLength : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "b" ) );
            client().update( ns(), BSON( "a" << "b" ), BSON( "$set" << BSON( "a" << "cd" ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << "cd" ) ).isEmpty() );
        }
    };

    class SetStringToNum : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "b" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a" << 5 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << 5 ) ).isEmpty() );
        }
    };

    class SetStringToNumInPlace : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "bcd" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a" << 5.0 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << 5.0 ) ).isEmpty() );
        }
    };

    class ModDotted : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{a:{b:4}}" ) );
            client().update( ns(), Query(), BSON( "$inc" << BSON( "a.b" << 10 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a.b" << 14 ) ).isEmpty() );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b" << 55 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a.b" << 55 ) ).isEmpty() );
        }
    };

    class SetInPlaceDotted : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{a:{b:'cdef'}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b" << "llll" ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a.b" << "llll" ) ).isEmpty() );
        }
    };

    class SetRecreateDotted : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:'cdef'}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b" << "lllll" ) ) );
            ASSERT( client().findOne( ns(), BSON( "a.b" << "lllll" ) ).woCompare( fromjson( "{'_id':0,a:{b:'lllll'}}" ) ) == 0 );
        }
    };

    class SetMissingDotted : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "a.b" << "lllll" ) ) );
            ASSERT( client().findOne( ns(), BSON( "a.b" << "lllll" ) ).woCompare( fromjson( "{'_id':0,a:{b:'lllll'}}" ) ) == 0 );
        }
    };

    class SetAdjacentDotted : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{c:4}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b" << "lllll" ) ) );
            ASSERT_EQUALS( client().findOne( ns(), BSON( "a.b" << "lllll" ) ) , fromjson( "{'_id':0,a:{b:'lllll',c:4}}" ) );
        }
    };

    class IncMissing : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), Query(), BSON( "$inc" << BSON( "f" << 3.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,f:3}" ) ) == 0 );
        }
    };

    class MultiInc : public SetBase {
    public:

        string s() {
            stringstream ss;
            auto_ptr<DBClientCursor> cc = client().query( ns() , Query().sort( BSON( "_id" << 1 ) ) );
            bool first = true;
            while ( cc->more() ) {
                if ( first ) first = false;
                else ss << ",";

                BSONObj o = cc->next();
                ss << o["x"].numberInt();
            }
            return ss.str();
        }

        void run() {
            client().insert( ns(), BSON( "_id" << 1 << "x" << 1 ) );
            client().insert( ns(), BSON( "_id" << 2 << "x" << 5 ) );

            ASSERT_EQUALS( "1,5" , s() );

            client().update( ns() , BSON( "_id" << 1 ) , BSON( "$inc" << BSON( "x" << 1 ) ) );
            ASSERT_EQUALS( "2,5" , s() );

            client().update( ns() , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) );
            ASSERT_EQUALS( "3,5" , s() );

            client().update( ns() , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) , false , true );
            ASSERT_EQUALS( "4,6" , s() );

        }
    };

    class UnorderedNewSet : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "f.g.h" << 3.0 << "f.g.a" << 2.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,f:{g:{a:2,h:3}}}" ) ) == 0 );
        }
    };

    class UnorderedNewSetAdjacent : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "f.g.h.b" << 3.0 << "f.g.a.b" << 2.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,f:{g:{a:{b:2},h:{b:3}}}}" ) ) == 0 );
        }
    };

    class ArrayEmbeddedSet : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,z:[4,'b']}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "z.0" << "a" ) ) );
            ASSERT_EQUALS( client().findOne( ns(), Query() ) , fromjson( "{'_id':0,z:['a','b']}" ) );
        }
    };

    class AttemptEmbedInExistingNum : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:1}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b" << 1 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:1}" ) ) == 0 );
        }
    };

    class AttemptEmbedConflictsWithOtherSet : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a" << 2 << "a.b" << 1 ) ) );
            ASSERT_EQUALS( client().findOne( ns(), Query() ) , fromjson( "{'_id':0}" ) );
        }
    };

    class ModMasksEmbeddedConflict : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:2}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a" << 2 << "a.b" << 1 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{b:2}}" ) ) == 0 );
        }
    };

    class ModOverwritesExistingObject : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:2}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a" << BSON( "c" << 2 ) ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{c:2}}" ) ) == 0 );
        }
    };

    class InvalidEmbeddedSet : public Fail {
    public:
        virtual void doIt() {
            client().update( ns(), Query(), BSON( "$set" << BSON( "a." << 1 ) ) );
        }
    };

    class UpsertMissingEmbedded : public SetBase {
    public:
        void run() {
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b" << 1 ) ), true );
            ASSERT( !client().findOne( ns(), QUERY( "a.b" << 1 ) ).isEmpty() );
        }
    };

    class Push : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:[1]}" ) );
            client().update( ns(), Query(), BSON( "$push" << BSON( "a" << 5 ) ) );
            ASSERT_EQUALS( client().findOne( ns(), Query() ) , fromjson( "{'_id':0,a:[1,5]}" ) );
        }
    };

    class PushInvalidEltType : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:1}" ) );
            client().update( ns(), Query(), BSON( "$push" << BSON( "a" << 5 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:1}" ) ) == 0 );
        }
    };

    class PushConflictsWithOtherMod : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:[1]}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a" << 1 ) <<"$push" << BSON( "a" << 5 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:[1]}" ) ) == 0 );
        }
    };

    class PushFromNothing : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), Query(), BSON( "$push" << BSON( "a" << 5 ) ) );
            ASSERT_EQUALS( client().findOne( ns(), Query() ) , fromjson( "{'_id':0,a:[5]}" ) );
        }
    };

    class PushFromEmpty : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:[]}" ) );
            client().update( ns(), Query(), BSON( "$push" << BSON( "a" << 5 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:[5]}" ) ) == 0 );
        }
    };

    class PushInsideNothing : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), Query(), BSON( "$push" << BSON( "a.b" << 5 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{b:[5]}}" ) ) == 0 );
        }
    };

    class CantPushInsideOtherMod : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a" << BSONObj() ) << "$push" << BSON( "a.b" << 5 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0}" ) ) == 0 );
        }
    };

    class CantPushTwice : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:[]}" ) );
            client().update( ns(), Query(), BSON( "$push" << BSON( "a" << 4 ) << "$push" << BSON( "a" << 5 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:[]}" ) ) == 0 );
        }
    };

    class SetEncapsulationConflictsWithExistingType : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:4}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b.c" << 4.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{b:4}}" ) ) == 0 );
        }
    };

    class CantPushToParent : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:4}}" ) );
            client().update( ns(), Query(), BSON( "$push" << BSON( "a" << 4.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{b:4}}" ) ) == 0 );
        }
    };

    class CantIncParent : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:4}}" ) );
            client().update( ns(), Query(), BSON( "$inc" << BSON( "a" << 4.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{b:4}}" ) ) == 0 );
        }
    };

    class DontDropEmpty : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:{}}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.c" << 4.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{b:{},c:4}}" ) ) == 0 );
        }
    };

    class InsertInEmpty : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{'_id':0,a:{b:{}}}" ) );
            client().update( ns(), Query(), BSON( "$set" << BSON( "a.b.f" << 4.0 ) ) );
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:{b:{f:4}}}" ) ) == 0 );
        }
    };

    class IndexParentOfMod : public SetBase {
    public:
        void run() {
            client().ensureIndex( ns(), BSON( "a" << 1 ) );
            client().insert( ns(), fromjson( "{'_id':0}" ) );
            client().update( ns(), Query(), fromjson( "{$set:{'a.b':4}}" ) );
            ASSERT_EQUALS( fromjson( "{'_id':0,a:{b:4}}" ) , client().findOne( ns(), Query() ) );
            ASSERT_EQUALS( fromjson( "{'_id':0,a:{b:4}}" ) , client().findOne( ns(), fromjson( "{'a.b':4}" ) ) ); // make sure the index works
        }
    };

    class IndexModSet : public SetBase {
    public:
        void run() {
            client().ensureIndex( ns(), BSON( "a.b" << 1 ) );
            client().insert( ns(), fromjson( "{'_id':0,a:{b:3}}" ) );
            client().update( ns(), Query(), fromjson( "{$set:{'a.b':4}}" ) );
            ASSERT_EQUALS( fromjson( "{'_id':0,a:{b:4}}" ) , client().findOne( ns(), Query() ) );
            ASSERT_EQUALS( fromjson( "{'_id':0,a:{b:4}}" ) , client().findOne( ns(), fromjson( "{'a.b':4}" ) ) ); // make sure the index works
        }
    };


    class PreserveIdWithIndex : public SetBase { // Not using $set, but base class is still useful
    public:
        void run() {
            client().insert( ns(), BSON( "_id" << 55 << "i" << 5 ) );
            client().update( ns(), BSON( "i" << 5 ), BSON( "i" << 6 ) );
            ASSERT( !client().findOne( ns(), Query( BSON( "_id" << 55 ) ).hint
                                       ( "{\"_id\":ObjectId(\"000000000000000000000000\")}" ) ).isEmpty() );
        }
    };

    class CheckNoMods : public SetBase {
    public:
        void run() {
            client().update( ns(), BSONObj(), BSON( "i" << 5 << "$set" << BSON( "q" << 3 ) ), true );
            ASSERT( error() );
        }
    };

    class UpdateMissingToNull : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << 5 ) );
            client().update( ns(), BSON( "a" << 5 ), fromjson( "{$set:{b:null}}" ) );
            ASSERT_EQUALS( jstNULL, client().findOne( ns(), QUERY( "a" << 5 ) ).getField( "b" ).type() );
        }
    };

    /** SERVER-4777 */
    class TwoModsWithinDuplicatedField : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "_id" << 0 << "a" << 1
                                        << "x" << BSONObj() << "x" << BSONObj()
                                        << "z" << 5 ) );
            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "x.b" << 1 << "x.c" << 1 ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1
                                << "x" << BSON( "b" << 1 << "c" << 1 ) << "x" << BSONObj()
                                << "z" << 5 ),
                          client().findOne( ns(), BSONObj() ) );
        }
    };

    /** SERVER-4777 */
    class ThreeModsWithinDuplicatedField : public SetBase {
    public:
        void run() {
            client().insert( ns(),
                            BSON( "_id" << 0
                                 << "x" << BSONObj() << "x" << BSONObj() << "x" << BSONObj() ) );
            client().update( ns(), BSONObj(),
                            BSON( "$set" << BSON( "x.b" << 1 << "x.c" << 1 << "x.d" << 1 ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0
                                << "x" << BSON( "b" << 1 << "c" << 1 << "d" << 1 )
                                << "x" << BSONObj() << "x" << BSONObj() ),
                          client().findOne( ns(), BSONObj() ) );
        }
    };

    class TwoModsBeforeExistingField : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "_id" << 0 << "x" << 5 ) );
            client().update( ns(), BSONObj(),
                            BSON( "$set" << BSON( "a" << 1 << "b" << 1 << "x" << 10 ) ) );
            ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 << "b" << 1 << "x" << 10 ),
                          client().findOne( ns(), BSONObj() ) );
        }
    };
    
    namespace ModSetTests {

        class internal1 {
        public:
            void run() {
                BSONObj b = BSON( "$inc" << BSON( "x" << 1 << "a.b" << 1 ) );
                ModSet m(b);

                ASSERT( m.haveModForField( "x" ) );
                ASSERT( m.haveModForField( "a.b" ) );
                ASSERT( ! m.haveModForField( "y" ) );
                ASSERT( ! m.haveModForField( "a.c" ) );
                ASSERT( ! m.haveModForField( "a" ) );

                ASSERT( m.haveConflictingMod( "x" ) );
                ASSERT( m.haveConflictingMod( "a" ) );
                ASSERT( m.haveConflictingMod( "a.b" ) );
                ASSERT( ! m.haveConflictingMod( "a.bc" ) );
                ASSERT( ! m.haveConflictingMod( "a.c" ) );
                ASSERT( ! m.haveConflictingMod( "a.a" ) );
            }
        };

        class Base {
        public:

            virtual ~Base() {}


            void test( BSONObj morig , BSONObj in , BSONObj wanted ) {
                BSONObj m = morig.copy();
                ModSet set(m);

                BSONObj out = set.prepare(in)->createNewFromMods();
                ASSERT_EQUALS( wanted , out );
            }
        };

        class inc1 : public Base {
        public:
            void run() {
                BSONObj m = BSON( "$inc" << BSON( "x" << 1 ) );
                test( m , BSON( "x" << 5 )  , BSON( "x" << 6 ) );
                test( m , BSON( "a" << 5 )  , BSON( "a" << 5 << "x" << 1 ) );
                test( m , BSON( "z" << 5 )  , BSON( "x" << 1 << "z" << 5 ) );
            }
        };

        class inc2 : public Base {
        public:
            void run() {
                BSONObj m = BSON( "$inc" << BSON( "a.b" << 1 ) );
                test( m , BSONObj() , BSON( "a" << BSON( "b" << 1 ) ) );
                test( m , BSON( "a" << BSON( "b" << 2 ) ) , BSON( "a" << BSON( "b" << 3 ) ) );

                m = BSON( "$inc" << BSON( "a.b" << 1 << "a.c" << 1 ) );
                test( m , BSONObj() , BSON( "a" << BSON( "b" << 1 << "c" << 1 ) ) );


            }
        };

        class set1 : public Base {
        public:
            void run() {
                test( BSON( "$set" << BSON( "x" << 17 ) ) , BSONObj() , BSON( "x" << 17 ) );
                test( BSON( "$set" << BSON( "x" << 17 ) ) , BSON( "x" << 5 ) , BSON( "x" << 17 ) );

                test( BSON( "$set" << BSON( "x.a" << 17 ) ) , BSON( "z" << 5 ) , BSON( "x" << BSON( "a" << 17 )<< "z" << 5 ) );
            }
        };

        class push1 : public Base {
        public:
            void run() {
                test( BSON( "$push" << BSON( "a" << 5 ) ) , fromjson( "{a:[1]}" ) , fromjson( "{a:[1,5]}" ) );
            }
        };
        
        class IncRewrite {
        public:
            void run() {
                BSONObj obj = BSON( "a" << 2 );
                BSONObj mod = BSON( "$inc" << BSON( "a" << 1 ) );
                ModSet modSet( mod );
                auto_ptr<ModSetState> modSetState = modSet.prepare( obj );
                modSetState->createNewFromMods();
                ASSERT( modSetState->needOpLogRewrite() );
                ASSERT_EQUALS( BSON( "$set" << BSON( "a" << 3 ) ), modSetState->getOpLogRewrite() );
            }
        };
   
        class IncRewriteNestedArray {
        public:
            void run() {
                BSONObj obj = BSON( "a" << BSON_ARRAY( 2 ) );
                BSONObj mod = BSON( "$inc" << BSON( "a.0" << 1 ) );
                ModSet modSet( mod );
                auto_ptr<ModSetState> modSetState = modSet.prepare( obj );
                modSetState->createNewFromMods();
                ASSERT( modSetState->needOpLogRewrite() );
                ASSERT_EQUALS( BSON( "$set" << BSON( "a.0" << 3 ) ),
                              modSetState->getOpLogRewrite() );
            }
        };

    };

    namespace basic {
        class Base : public ClientBase {
        protected:

            virtual const char * ns() = 0;
            virtual void dotest() = 0;

            void insert( const BSONObj& o ) {
                client().insert( ns() , o );
            }

            void update( const BSONObj& m ) {
                client().update( ns() , BSONObj() , m );
            }

            BSONObj findOne() {
                return client().findOne( ns() , BSONObj() );
            }

            void test( const char* initial , const char* mod , const char* after ) {
                test( fromjson( initial ) , fromjson( mod ) , fromjson( after ) );
            }


            void test( const BSONObj& initial , const BSONObj& mod , const BSONObj& after ) {
                client().dropCollection( ns() );
                insert( initial );
                update( mod );
                ASSERT_EQUALS( after , findOne() );
                client().dropCollection( ns() );
            }

        public:

            Base() {}
            virtual ~Base() {
            }

            void run() {
                client().dropCollection( ns() );

                dotest();

                client().dropCollection( ns() );
            }
        };

        class SingleTest : public Base {
            virtual BSONObj initial() = 0;
            virtual BSONObj mod() = 0;
            virtual BSONObj after() = 0;

            void dotest() {
                test( initial() , mod() , after() );
            }

        };

        class inc1 : public SingleTest {
            virtual BSONObj initial() {
                return BSON( "_id" << 1 << "x" << 1 );
            }
            virtual BSONObj mod() {
                return BSON( "$inc" << BSON( "x" << 2 ) );
            }
            virtual BSONObj after() {
                return BSON( "_id" << 1 << "x" << 3 );
            }
            virtual const char * ns() {
                return "unittests.inc1";
            }

        };

        class inc2 : public SingleTest {
            virtual BSONObj initial() {
                return BSON( "_id" << 1 << "x" << 1 );
            }
            virtual BSONObj mod() {
                return BSON( "$inc" << BSON( "x" << 2.5 ) );
            }
            virtual BSONObj after() {
                return BSON( "_id" << 1 << "x" << 3.5 );
            }
            virtual const char * ns() {
                return "unittests.inc2";
            }

        };

        class inc3 : public SingleTest {
            virtual BSONObj initial() {
                return BSON( "_id" << 1 << "x" << 537142123123LL );
            }
            virtual BSONObj mod() {
                return BSON( "$inc" << BSON( "x" << 2 ) );
            }
            virtual BSONObj after() {
                return BSON( "_id" << 1 << "x" << 537142123125LL );
            }
            virtual const char * ns() {
                return "unittests.inc3";
            }

        };

        class inc4 : public SingleTest {
            virtual BSONObj initial() {
                return BSON( "_id" << 1 << "x" << 537142123123LL );
            }
            virtual BSONObj mod() {
                return BSON( "$inc" << BSON( "x" << 2LL ) );
            }
            virtual BSONObj after() {
                return BSON( "_id" << 1 << "x" << 537142123125LL );
            }
            virtual const char * ns() {
                return "unittests.inc4";
            }

        };

        class inc5 : public SingleTest {
            virtual BSONObj initial() {
                return BSON( "_id" << 1 << "x" << 537142123123LL );
            }
            virtual BSONObj mod() {
                return BSON( "$inc" << BSON( "x" << 2.0 ) );
            }
            virtual BSONObj after() {
                return BSON( "_id" << 1 << "x" << 537142123125LL );
            }
            virtual const char * ns() {
                return "unittests.inc5";
            }

        };

        class inc6 : public Base {

            virtual const char * ns() {
                return "unittests.inc6";
            }


            virtual BSONObj initial() { return BSONObj(); }
            virtual BSONObj mod() { return BSONObj(); }
            virtual BSONObj after() { return BSONObj(); }

            void dotest() {
                long long start = numeric_limits<int>::max() - 5;
                long long max   = numeric_limits<int>::max() + 5ll;

                client().insert( ns() , BSON( "x" << (int)start ) );
                ASSERT( findOne()["x"].type() == NumberInt );

                while ( start < max ) {
                    update( BSON( "$inc" << BSON( "x" << 1 ) ) );
                    start += 1;
                    ASSERT_EQUALS( start , findOne()["x"].numberLong() ); // SERVER-2005
                }

                ASSERT( findOne()["x"].type() == NumberLong );
            }
        };

        class bit1 : public Base {
            const char * ns() {
                return "unittests.bit1";
            }
            void dotest() {
                test( BSON( "_id" << 1 << "x" << 3 ) , BSON( "$bit" << BSON( "x" << BSON( "and" << 2 ) ) ) , BSON( "_id" << 1 << "x" << ( 3 & 2 ) ) );
                test( BSON( "_id" << 1 << "x" << 1 ) , BSON( "$bit" << BSON( "x" << BSON( "or" << 4 ) ) ) , BSON( "_id" << 1 << "x" << ( 1 | 4 ) ) );
                test( BSON( "_id" << 1 << "x" << 3 ) , BSON( "$bit" << BSON( "x" << BSON( "and" << 2 << "or" << 8 ) ) ) , BSON( "_id" << 1 << "x" << ( ( 3 & 2 ) | 8 ) ) );
                test( BSON( "_id" << 1 << "x" << 3 ) , BSON( "$bit" << BSON( "x" << BSON( "or" << 2 << "and" << 8 ) ) ) , BSON( "_id" << 1 << "x" << ( ( 3 | 2 ) & 8 ) ) );

            }
        };

        class unset : public Base {
            const char * ns() {
                return "unittests.unset";
            }
            void dotest() {
                test( "{_id:1,x:1}" , "{$unset:{x:1}}" , "{_id:1}" );
            }
        };

        class setswitchint : public Base {
            const char * ns() {
                return "unittests.int1";
            }
            void dotest() {
                test( BSON( "_id" << 1 << "x" << 1 ) , BSON( "$set" << BSON( "x" << 5.6 ) ) , BSON( "_id" << 1 << "x" << 5.6 ) );
                test( BSON( "_id" << 1 << "x" << 5.6 ) , BSON( "$set" << BSON( "x" << 1 ) ) , BSON( "_id" << 1 << "x" << 1 ) );
            }
        };


    };

    class All : public Suite {
    public:
        All() : Suite( "update" ) {
        }
        void setupTests() {
            add< ModId >();
            add< ModNonmodMix >();
            add< InvalidMod >();
            add< ModNotFirst >();
            add< ModDuplicateFieldSpec >();
            add< IncNonNumber >();
            add< PushAllNonArray >();
            add< PullAllNonArray >();
            add< IncTargetNonNumber >();
            add< SetNum >();
            add< SetString >();
            add< SetStringDifferentLength >();
            add< SetStringToNum >();
            add< SetStringToNumInPlace >();
            add< ModDotted >();
            add< SetInPlaceDotted >();
            add< SetRecreateDotted >();
            add< SetMissingDotted >();
            add< SetAdjacentDotted >();
            add< IncMissing >();
            add< MultiInc >();
            add< UnorderedNewSet >();
            add< UnorderedNewSetAdjacent >();
            add< ArrayEmbeddedSet >();
            add< AttemptEmbedInExistingNum >();
            add< AttemptEmbedConflictsWithOtherSet >();
            add< ModMasksEmbeddedConflict >();
            add< ModOverwritesExistingObject >();
            add< InvalidEmbeddedSet >();
            add< UpsertMissingEmbedded >();
            add< Push >();
            add< PushInvalidEltType >();
            add< PushConflictsWithOtherMod >();
            add< PushFromNothing >();
            add< PushFromEmpty >();
            add< PushInsideNothing >();
            add< CantPushInsideOtherMod >();
            add< CantPushTwice >();
            add< SetEncapsulationConflictsWithExistingType >();
            add< CantPushToParent >();
            add< CantIncParent >();
            add< DontDropEmpty >();
            add< InsertInEmpty >();
            add< IndexParentOfMod >();
            add< IndexModSet >();
            add< PreserveIdWithIndex >();
            add< CheckNoMods >();
            add< UpdateMissingToNull >();
            add< TwoModsWithinDuplicatedField >();
            add< ThreeModsWithinDuplicatedField >();
            add< TwoModsBeforeExistingField >();

            add< ModSetTests::internal1 >();
            add< ModSetTests::inc1 >();
            add< ModSetTests::inc2 >();
            add< ModSetTests::set1 >();
            add< ModSetTests::push1 >();
            add< ModSetTests::IncRewrite >();
            add< ModSetTests::IncRewriteNestedArray >();

            add< basic::inc1 >();
            add< basic::inc2 >();
            add< basic::inc3 >();
            add< basic::inc4 >();
            add< basic::inc5 >();
            add< basic::inc6 >();
            add< basic::bit1 >();
            add< basic::unset >();
            add< basic::setswitchint >();
        }
    } myall;

} // namespace UpdateTests

