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

#include "../db/query.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"

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
        const char *ns() { return "UpdateTests_Fail"; }
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
        const char *ns() { return "updatetests.SetBase"; }
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
            ASSERT( client().findOne( ns(), BSON( "a.b" << "lllll" ) ).woCompare( fromjson( "{'_id':0,a:{b:'lllll',c:4}}" ) ) == 0 );                        
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
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,z:[4,'b']}" ) ) == 0 );                        
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
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0}" ) ) == 0 );         
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
            ASSERT( client().findOne( ns(), Query() ).woCompare( fromjson( "{'_id':0,a:[1,5]}" ) ) == 0 );                     
        }
    };
    
    class All : public UnitTest::Suite {
    public:
        All() {
            add< ModId >();
            add< ModNonmodMix >();
            add< InvalidMod >();
            add< ModNotFirst >();
            add< ModDuplicateFieldSpec >();
            add< IncNonNumber >();
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
        }
    };
    
} // namespace UpdateTests

UnitTest::TestPtr updateTests() {
    return UnitTest::createSuite< UpdateTests::All >();
}