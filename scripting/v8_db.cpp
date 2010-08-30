// v8_db.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "v8_wrapper.h"
#include "v8_utils.h"
#include "v8_db.h"
#include "engine.h"
#include "util/base64.h"
#include "util/text.h"
#include "../client/syncclusterconnection.h"
#include <iostream>

using namespace std;
using namespace v8;

namespace mongo {

#define DDD(x)

    v8::Handle<v8::FunctionTemplate> getMongoFunctionTemplate( bool local ){
        v8::Local<v8::FunctionTemplate> mongo = FunctionTemplate::New( local ? mongoConsLocal : mongoConsExternal );
        mongo->InstanceTemplate()->SetInternalFieldCount( 1 );
        
        v8::Local<v8::Template> proto = mongo->PrototypeTemplate();

        proto->Set( v8::String::New( "find" ) , FunctionTemplate::New( mongoFind ) );
        proto->Set( v8::String::New( "insert" ) , FunctionTemplate::New( mongoInsert ) );
        proto->Set( v8::String::New( "remove" ) , FunctionTemplate::New( mongoRemove ) );
        proto->Set( v8::String::New( "update" ) , FunctionTemplate::New( mongoUpdate ) );

        Local<FunctionTemplate> ic = FunctionTemplate::New( internalCursorCons );
        ic->InstanceTemplate()->SetInternalFieldCount( 1 );
        ic->PrototypeTemplate()->Set( v8::String::New("next") , FunctionTemplate::New( internalCursorNext ) );
        ic->PrototypeTemplate()->Set( v8::String::New("hasNext") , FunctionTemplate::New( internalCursorHasNext ) );
        ic->PrototypeTemplate()->Set( v8::String::New("objsLeftInBatch") , FunctionTemplate::New( internalCursorObjsLeftInBatch ) );
        proto->Set( v8::String::New( "internalCursor" ) , ic );
        


        return mongo;
    }

    v8::Handle<v8::FunctionTemplate> getNumberLongFunctionTemplate() {
        v8::Local<v8::FunctionTemplate> numberLong = FunctionTemplate::New( numberLongInit );
        v8::Local<v8::Template> proto = numberLong->PrototypeTemplate();
        
        proto->Set( v8::String::New( "valueOf" ) , FunctionTemplate::New( numberLongValueOf ) );        
        proto->Set( v8::String::New( "toNumber" ) , FunctionTemplate::New( numberLongToNumber ) );        
        proto->Set( v8::String::New( "toString" ) , FunctionTemplate::New( numberLongToString ) );
        
        return numberLong;
    }

    v8::Handle<v8::FunctionTemplate> getBinDataFunctionTemplate() {
        v8::Local<v8::FunctionTemplate> binData = FunctionTemplate::New( binDataInit );
        v8::Local<v8::Template> proto = binData->PrototypeTemplate();
        
        proto->Set( v8::String::New( "toString" ) , FunctionTemplate::New( binDataToString ) );        
        
        return binData;
    }    
    
    void installDBTypes( Handle<ObjectTemplate>& global ){
        v8::Local<v8::FunctionTemplate> db = FunctionTemplate::New( dbInit );
        db->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );
        global->Set(v8::String::New("DB") , db );
        
        v8::Local<v8::FunctionTemplate> dbCollection = FunctionTemplate::New( collectionInit );
        dbCollection->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );
        global->Set(v8::String::New("DBCollection") , dbCollection );


        v8::Local<v8::FunctionTemplate> dbQuery = FunctionTemplate::New( dbQueryInit );
        dbQuery->InstanceTemplate()->SetIndexedPropertyHandler( dbQueryIndexAccess );
        global->Set(v8::String::New("DBQuery") , dbQuery );

        global->Set( v8::String::New("ObjectId") , FunctionTemplate::New( objectIdInit ) );

        global->Set( v8::String::New("DBRef") , FunctionTemplate::New( dbRefInit ) );

        global->Set( v8::String::New("DBPointer") , FunctionTemplate::New( dbPointerInit ) );

        global->Set( v8::String::New("BinData") , getBinDataFunctionTemplate() );

        global->Set( v8::String::New("NumberLong") , getNumberLongFunctionTemplate() );

    }

    void installDBTypes( Handle<v8::Object>& global ){
        v8::Local<v8::FunctionTemplate> db = FunctionTemplate::New( dbInit );
        db->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );
        global->Set(v8::String::New("DB") , db->GetFunction() );
        
        v8::Local<v8::FunctionTemplate> dbCollection = FunctionTemplate::New( collectionInit );
        dbCollection->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );
        global->Set(v8::String::New("DBCollection") , dbCollection->GetFunction() );


        v8::Local<v8::FunctionTemplate> dbQuery = FunctionTemplate::New( dbQueryInit );
        dbQuery->InstanceTemplate()->SetIndexedPropertyHandler( dbQueryIndexAccess );
        global->Set(v8::String::New("DBQuery") , dbQuery->GetFunction() );

        global->Set( v8::String::New("ObjectId") , FunctionTemplate::New( objectIdInit )->GetFunction() );

        global->Set( v8::String::New("DBRef") , FunctionTemplate::New( dbRefInit )->GetFunction() );
        
        global->Set( v8::String::New("DBPointer") , FunctionTemplate::New( dbPointerInit )->GetFunction() );

        global->Set( v8::String::New("BinData") , getBinDataFunctionTemplate()->GetFunction() );

        global->Set( v8::String::New("NumberLong") , getNumberLongFunctionTemplate()->GetFunction() );

        BSONObjBuilder b;
        b.appendMaxKey( "" );
        b.appendMinKey( "" );
        BSONObj o = b.obj();
        BSONObjIterator i( o );
        global->Set( v8::String::New("MaxKey"), mongoToV8Element( i.next() ) );
        global->Set( v8::String::New("MinKey"), mongoToV8Element( i.next() ) );
        
        global->Get( v8::String::New( "Object" ) )->ToObject()->Set( v8::String::New("bsonsize") , FunctionTemplate::New( bsonsize )->GetFunction() );
    }

    void destroyConnection( Persistent<Value> self, void* parameter){
        delete static_cast<DBClientBase*>(parameter);
        self.Dispose();
        self.Clear();
    }

    Handle<Value> mongoConsExternal(const Arguments& args){

        char host[255];
    
        if ( args.Length() > 0 && args[0]->IsString() ){
            assert( args[0]->ToString()->Utf8Length() < 250 );
            args[0]->ToString()->WriteAscii( host );
        }
        else {
            strcpy( host , "127.0.0.1" );
        }

        string errmsg;
        ConnectionString cs = ConnectionString::parse( host , errmsg );
        if ( ! cs.isValid() )
            return v8::ThrowException( v8::String::New( errmsg.c_str() ) );
        
        
        DBClientWithCommands * conn = cs.connect( errmsg );
        if ( ! conn )
            return v8::ThrowException( v8::String::New( errmsg.c_str() ) );
        
        Persistent<v8::Object> self = Persistent<v8::Object>::New( args.Holder() );
        self.MakeWeak( conn , destroyConnection );

        ScriptEngine::runConnectCallback( *conn );

        args.This()->SetInternalField( 0 , External::New( conn ) );
        args.This()->Set( v8::String::New( "slaveOk" ) , Boolean::New( false ) );
        args.This()->Set( v8::String::New( "host" ) , v8::String::New( host ) );
    
        return v8::Undefined();
    }

    Handle<Value> mongoConsLocal(const Arguments& args){
        
        if ( args.Length() > 0 )
            return v8::ThrowException( v8::String::New( "local Mongo constructor takes no args" ) );

        DBClientBase * conn = createDirectClient();

        Persistent<v8::Object> self = Persistent<v8::Object>::New( args.This() );
        self.MakeWeak( conn , destroyConnection );

        // NOTE I don't believe the conn object will ever be freed.
        args.This()->SetInternalField( 0 , External::New( conn ) );
        args.This()->Set( v8::String::New( "slaveOk" ) , Boolean::New( false ) );
        args.This()->Set( v8::String::New( "host" ) , v8::String::New( "EMBEDDED" ) );
        
        return v8::Undefined();
    }


    // ---

#ifdef _WIN32
#define GETNS char * ns = new char[args[0]->ToString()->Utf8Length()];  args[0]->ToString()->WriteUtf8( ns ); 
#else
#define GETNS char ns[args[0]->ToString()->Utf8Length()];  args[0]->ToString()->WriteUtf8( ns ); 
#endif

    DBClientBase * getConnection( const Arguments& args ){
        Local<External> c = External::Cast( *(args.This()->GetInternalField( 0 )) );
        DBClientBase * conn = (DBClientBase*)(c->Value());
        assert( conn );
        return conn;
    }

    // ---- real methods

    void destroyCursor( Persistent<Value> self, void* parameter){
        delete static_cast<mongo::DBClientCursor*>(parameter);
        self.Dispose();
        self.Clear();
    }

    /**
       0 - namespace
       1 - query
       2 - fields
       3 - limit
       4 - skip
    */
    Handle<Value> mongoFind(const Arguments& args){
        HandleScope handle_scope;

        jsassert( args.Length() == 6 , "find needs 6 args" );
        jsassert( args[1]->IsObject() , "needs to be an object" );
        DBClientBase * conn = getConnection( args );
        GETNS;

        BSONObj q = v8ToMongo( args[1]->ToObject() );
        DDD( "query:" << q  );
    
        BSONObj fields;
        bool haveFields = args[2]->IsObject() && args[2]->ToObject()->GetPropertyNames()->Length() > 0;
        if ( haveFields )
            fields = v8ToMongo( args[2]->ToObject() );
    
        Local<v8::Object> mongo = args.This();
        Local<v8::Value> slaveOkVal = mongo->Get( v8::String::New( "slaveOk" ) );
        jsassert( slaveOkVal->IsBoolean(), "slaveOk member invalid" );
        bool slaveOk = slaveOkVal->BooleanValue();
        
        try {
            auto_ptr<mongo::DBClientCursor> cursor;
            int nToReturn = (int)(args[3]->ToNumber()->Value());
            int nToSkip = (int)(args[4]->ToNumber()->Value());
            int batchSize = (int)(args[5]->ToNumber()->Value());
            {
                v8::Unlocker u;
                cursor = conn->query( ns, q ,  nToReturn , nToSkip , haveFields ? &fields : 0, slaveOk ? QueryOption_SlaveOk : 0 , batchSize );
            }
            v8::Function * cons = (v8::Function*)( *( mongo->Get( v8::String::New( "internalCursor" ) ) ) );
            assert( cons );
            
            Persistent<v8::Object> c = Persistent<v8::Object>::New( cons->NewInstance() );
            c.MakeWeak( cursor.get() , destroyCursor );
            
            c->SetInternalField( 0 , External::New( cursor.release() ) );
            return handle_scope.Close(c);
        }
        catch ( ... ){
            return v8::ThrowException( v8::String::New( "socket error on query" ) );        
        }
    }

    v8::Handle<v8::Value> mongoInsert(const v8::Arguments& args){
        jsassert( args.Length() == 2 , "insert needs 2 args" );
        jsassert( args[1]->IsObject() , "have to insert an object" );
    
        DBClientBase * conn = getConnection( args );
        GETNS;
    
        v8::Handle<v8::Object> in = args[1]->ToObject();
    
        if ( ! in->Has( v8::String::New( "_id" ) ) ){
            v8::Handle<v8::Value> argv[1];
            in->Set( v8::String::New( "_id" ) , getObjectIdCons()->NewInstance( 0 , argv ) );
        }

        BSONObj o = v8ToMongo( in );

        DDD( "want to save : " << o.jsonString() );
        try {
            v8::Unlocker u;
            conn->insert( ns , o );
        }
        catch ( ... ){
            return v8::ThrowException( v8::String::New( "socket error on insert" ) );
        }
    
        return v8::Undefined();
    }

    v8::Handle<v8::Value> mongoRemove(const v8::Arguments& args){
        jsassert( args.Length() == 2 || args.Length() == 3 , "remove needs 2 args" );
        jsassert( args[1]->IsObject() , "have to remove an object template" );

        DBClientBase * conn = getConnection( args );
        GETNS;
    
        v8::Handle<v8::Object> in = args[1]->ToObject();
        BSONObj o = v8ToMongo( in );
    
        bool justOne = false;
        if ( args.Length() > 2 ){
            justOne = args[2]->BooleanValue();
        }

        DDD( "want to remove : " << o.jsonString() );
        try {
            v8::Unlocker u;
            conn->remove( ns , o , justOne );
        }
        catch ( ... ){
            return v8::ThrowException( v8::String::New( "socket error on remove" ) );
        }

        return v8::Undefined();
    }

    v8::Handle<v8::Value> mongoUpdate(const v8::Arguments& args){
        jsassert( args.Length() >= 3 , "update needs at least 3 args" );
        jsassert( args[1]->IsObject() , "1st param to update has to be an object" );
        jsassert( args[2]->IsObject() , "2nd param to update has to be an object" );

        DBClientBase * conn = getConnection( args );
        GETNS;
    
        v8::Handle<v8::Object> q = args[1]->ToObject();
        v8::Handle<v8::Object> o = args[2]->ToObject();
    
        bool upsert = args.Length() > 3 && args[3]->IsBoolean() && args[3]->ToBoolean()->Value();
        bool multi = args.Length() > 4 && args[4]->IsBoolean() && args[4]->ToBoolean()->Value();        
        
        try {
            BSONObj q1 = v8ToMongo( q );
            BSONObj o1 = v8ToMongo( o );
            v8::Unlocker u;
            conn->update( ns , q1 , o1 , upsert, multi );
        }
        catch ( ... ){
            return v8::ThrowException( v8::String::New( "socket error on remove" ) );
        }

        return v8::Undefined();
    }




    // --- cursor ---

    mongo::DBClientCursor * getCursor( const Arguments& args ){
        Local<External> c = External::Cast( *(args.This()->GetInternalField( 0 ) ) );

        mongo::DBClientCursor * cursor = (mongo::DBClientCursor*)(c->Value());
        return cursor;
    }

    v8::Handle<v8::Value> internalCursorCons(const v8::Arguments& args){
        return v8::Undefined();
    }

    v8::Handle<v8::Value> internalCursorNext(const v8::Arguments& args){    
        mongo::DBClientCursor * cursor = getCursor( args );
        if ( ! cursor )
            return v8::Undefined();
        BSONObj o;
        {
            v8::Unlocker u;
            o = cursor->next();
        }
        return mongoToV8( o );
    }

    v8::Handle<v8::Value> internalCursorHasNext(const v8::Arguments& args){
        mongo::DBClientCursor * cursor = getCursor( args );
        if ( ! cursor )
            return Boolean::New( false );
        bool ret;
        {
            v8::Unlocker u;
            ret = cursor->more();
        }
        return Boolean::New( ret );
    }

    v8::Handle<v8::Value> internalCursorObjsLeftInBatch(const v8::Arguments& args){
        mongo::DBClientCursor * cursor = getCursor( args );
        if ( ! cursor )
            return v8::Number::New( (double) 0 );
        int ret;
        {
            v8::Unlocker u;
            ret = cursor->objsLeftInBatch();
        }
        return v8::Number::New( (double) ret );
    }


    // --- DB ----

    v8::Handle<v8::Value> dbInit(const v8::Arguments& args){
        assert( args.Length() == 2 );

        args.This()->Set( v8::String::New( "_mongo" ) , args[0] );
        args.This()->Set( v8::String::New( "_name" ) , args[1] );

        for ( int i=0; i<args.Length(); i++ )
            assert( ! args[i]->IsUndefined() );

        return v8::Undefined();
    }

    v8::Handle<v8::Value> collectionInit( const v8::Arguments& args ){
        assert( args.Length() == 4 );

        args.This()->Set( v8::String::New( "_mongo" ) , args[0] );
        args.This()->Set( v8::String::New( "_db" ) , args[1] );
        args.This()->Set( v8::String::New( "_shortName" ) , args[2] );
        args.This()->Set( v8::String::New( "_fullName" ) , args[3] );
    
        for ( int i=0; i<args.Length(); i++ )
            assert( ! args[i]->IsUndefined() );

        return v8::Undefined();
    }

    v8::Handle<v8::Value> dbQueryInit( const v8::Arguments& args ){
    
        v8::Handle<v8::Object> t = args.This();

        assert( args.Length() >= 4 );
    
        t->Set( v8::String::New( "_mongo" ) , args[0] );
        t->Set( v8::String::New( "_db" ) , args[1] );
        t->Set( v8::String::New( "_collection" ) , args[2] );
        t->Set( v8::String::New( "_ns" ) , args[3] );

        if ( args.Length() > 4 && args[4]->IsObject() )
            t->Set( v8::String::New( "_query" ) , args[4] );
        else 
            t->Set( v8::String::New( "_query" ) , v8::Object::New() );
    
        if ( args.Length() > 5 && args[5]->IsObject() )
            t->Set( v8::String::New( "_fields" ) , args[5] );
        else
            t->Set( v8::String::New( "_fields" ) , v8::Null() );
    

        if ( args.Length() > 6 && args[6]->IsNumber() )
            t->Set( v8::String::New( "_limit" ) , args[6] );
        else 
            t->Set( v8::String::New( "_limit" ) , Number::New( 0 ) );

        if ( args.Length() > 7 && args[7]->IsNumber() )
            t->Set( v8::String::New( "_skip" ) , args[7] );
        else 
            t->Set( v8::String::New( "_skip" ) , Number::New( 0 ) );

        if ( args.Length() > 8 && args[8]->IsNumber() )
            t->Set( v8::String::New( "_batchSize" ) , args[7] );
        else 
            t->Set( v8::String::New( "_batchSize" ) , Number::New( 0 ) );
    
        t->Set( v8::String::New( "_cursor" ) , v8::Null() );
        t->Set( v8::String::New( "_numReturned" ) , v8::Number::New(0) );
        t->Set( v8::String::New( "_special" ) , Boolean::New(false) );
    
        return v8::Undefined();
    }

    v8::Handle<v8::Value> collectionFallback( v8::Local<v8::String> name, const v8::AccessorInfo &info) {
        DDD( "collectionFallback [" << name << "]" );
    
        v8::Handle<v8::Value> real = info.This()->GetPrototype()->ToObject()->Get( name );
        if ( ! real->IsUndefined() )
            return real;
    
        string sname = toSTLString( name );
        if ( sname[0] == '_' ){
            if ( ! ( info.This()->HasRealNamedProperty( name ) ) )
                return v8::Undefined();
            return info.This()->GetRealNamedPropertyInPrototypeChain( name );
        }

        v8::Handle<v8::Value> getCollection = info.This()->GetPrototype()->ToObject()->Get( v8::String::New( "getCollection" ) );
        assert( getCollection->IsFunction() );

        v8::Function * f = (v8::Function*)(*getCollection);
        v8::Handle<v8::Value> argv[1];
        argv[0] = name;

        return f->Call( info.This() , 1 , argv );
    }

    v8::Handle<v8::Value> dbQueryIndexAccess( unsigned int index , const v8::AccessorInfo& info ){
        v8::Handle<v8::Value> arrayAccess = info.This()->GetPrototype()->ToObject()->Get( v8::String::New( "arrayAccess" ) );
        assert( arrayAccess->IsFunction() );

        v8::Function * f = (v8::Function*)(*arrayAccess);
        v8::Handle<v8::Value> argv[1];
        argv[0] = v8::Number::New( index );

        return f->Call( info.This() , 1 , argv );    
    }

    v8::Handle<v8::Value> objectIdInit( const v8::Arguments& args ){
        v8::Handle<v8::Object> it = args.This();
    
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ){
            v8::Function * f = getObjectIdCons();
            it = f->NewInstance();
        }
    
        OID oid;
    
        if ( args.Length() == 0 ){
            oid.init();
        }
        else {
            string s = toSTLString( args[0] );
            try {
                Scope::validateObjectIdString( s );
            } catch ( const MsgAssertionException &m ) {
                string error = m.toString();
                return v8::ThrowException( v8::String::New( error.c_str() ) );
            }            
            oid.init( s );
        } 

        it->Set( v8::String::New( "str" ) , v8::String::New( oid.str().c_str() ) );
   
        return it;
    }

    v8::Handle<v8::Value> dbRefInit( const v8::Arguments& args ) {

        if (args.Length() != 2 && args.Length() != 0) {
            return v8::ThrowException( v8::String::New( "DBRef needs 2 arguments" ) );
        }

        v8::Handle<v8::Object> it = args.This();

        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ){
            v8::Function* f = getNamedCons( "DBRef" );
            it = f->NewInstance();
        }

        if ( args.Length() == 2 ) {
            it->Set( v8::String::New( "$ref" ) , args[0] );
            it->Set( v8::String::New( "$id" ) , args[1] );
        }

        return it;
    }

    v8::Handle<v8::Value> dbPointerInit( const v8::Arguments& args ) {
        
        if (args.Length() != 2) {
            return v8::ThrowException( v8::String::New( "DBPointer needs 2 arguments" ) );
        }
        
        v8::Handle<v8::Object> it = args.This();
        
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ){
            v8::Function* f = getNamedCons( "DBPointer" );
            it = f->NewInstance();
        }
        
        it->Set( v8::String::New( "ns" ) , args[0] );
        it->Set( v8::String::New( "id" ) , args[1] );
        it->SetHiddenValue( v8::String::New( "__DBPointer" ), v8::Number::New( 1 ) );
        
        return it;
    }

    v8::Handle<v8::Value> binDataInit( const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        
        // 3 args: len, type, data
        if (args.Length() == 3) {
        
            if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ){
                v8::Function* f = getNamedCons( "BinData" );
                it = f->NewInstance();
            }
        
            it->Set( v8::String::New( "len" ) , args[0] );
            it->Set( v8::String::New( "type" ) , args[1] );
            it->Set( v8::String::New( "data" ), args[2] );
            it->SetHiddenValue( v8::String::New( "__BinData" ), v8::Number::New( 1 ) );

        // 2 args: type, base64 string
        } else if ( args.Length() == 2 ) {
            
            if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ){
                v8::Function* f = getNamedCons( "BinData" );
                it = f->NewInstance();
            }
            
            v8::String::Utf8Value data( args[ 1 ] );
            string decoded = base64::decode( *data );
            it->Set( v8::String::New( "len" ) , v8::Number::New( decoded.length() ) );
            it->Set( v8::String::New( "type" ) , args[ 0 ] );
            it->Set( v8::String::New( "data" ), v8::String::New( decoded.data(), decoded.length() ) );
            it->SetHiddenValue( v8::String::New( "__BinData" ), v8::Number::New( 1 ) );            
            
        } else {
            return v8::ThrowException( v8::String::New( "BinData needs 3 arguments" ) );
        }

        return it;
    }
    
    v8::Handle<v8::Value> binDataToString( const v8::Arguments& args ) {
        
        if (args.Length() != 0) {
            return v8::ThrowException( v8::String::New( "toString needs 0 arguments" ) );
        }
        
        v8::Handle<v8::Object> it = args.This();
        int len = it->Get( v8::String::New( "len" ) )->ToInt32()->Value();
        int type = it->Get( v8::String::New( "type" ) )->ToInt32()->Value();
        v8::String::Utf8Value data( it->Get( v8::String::New( "data" ) ) );
        
        stringstream ss;
        ss << "BinData(" << type << ",\"";
        base64::encode( ss, *data, len );
        ss << "\")";
        string ret = ss.str();
        return v8::String::New( ret.c_str() );
    }

    v8::Handle<v8::Value> numberLongInit( const v8::Arguments& args ) {
        
        if (args.Length() != 0 && args.Length() != 1 && args.Length() != 3) {
            return v8::ThrowException( v8::String::New( "NumberLong needs 0, 1 or 3 arguments" ) );
        }
        
        v8::Handle<v8::Object> it = args.This();
        
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ){
            v8::Function* f = getNamedCons( "NumberLong" );
            it = f->NewInstance();
        }

        if ( args.Length() == 0 ) {
            it->Set( v8::String::New( "floatApprox" ), v8::Number::New( 0 ) );
        } else if ( args.Length() == 1 ) {
            if ( args[ 0 ]->IsNumber() ) {
                it->Set( v8::String::New( "floatApprox" ), args[ 0 ] );            
            } else {
                v8::String::Utf8Value data( args[ 0 ] );
                string num = *data;
                const char *numStr = num.c_str();
                long long n;
                try {
                    n = parseLL( numStr );
                } catch ( const AssertionException & ) {
                    return v8::ThrowException( v8::String::New( "could not convert string to long long" ) );
                }
                unsigned long long val = n;
                if ( (long long)val == (long long)(double)(long long)(val) ) {
                    it->Set( v8::String::New( "floatApprox" ), v8::Number::New( (double)(long long)( val ) ) );
                } else {
                    it->Set( v8::String::New( "floatApprox" ), v8::Number::New( (double)(long long)( val ) ) );
                    it->Set( v8::String::New( "top" ), v8::Integer::New( val >> 32 ) );
                    it->Set( v8::String::New( "bottom" ), v8::Integer::New( (unsigned long)(val & 0x00000000ffffffff) ) );
                }                
            }
        } else {
            it->Set( v8::String::New( "floatApprox" ) , args[0] );
            it->Set( v8::String::New( "top" ) , args[1] );
            it->Set( v8::String::New( "bottom" ) , args[2] );
        }
        it->SetHiddenValue( v8::String::New( "__NumberLong" ), v8::Number::New( 1 ) );
        
        return it;
    }

    long long numberLongVal( const v8::Handle< v8::Object > &it ) {
        if ( !it->Has( v8::String::New( "top" ) ) )
            return (long long)( it->Get( v8::String::New( "floatApprox" ) )->NumberValue() );
        return
        (long long)
        ( (unsigned long long)( it->Get( v8::String::New( "top" ) )->ToInt32()->Value() ) << 32 ) +
        (unsigned)( it->Get( v8::String::New( "bottom" ) )->ToInt32()->Value() );        
    }
    
    v8::Handle<v8::Value> numberLongValueOf( const v8::Arguments& args ) {
        
        if (args.Length() != 0) {
            return v8::ThrowException( v8::String::New( "toNumber needs 0 arguments" ) );
        }
        
        v8::Handle<v8::Object> it = args.This();
        
        long long val = numberLongVal( it );
        
        return v8::Number::New( double( val ) );
    }

    v8::Handle<v8::Value> numberLongToNumber( const v8::Arguments& args ) {
        return numberLongValueOf( args );
    }

    v8::Handle<v8::Value> numberLongToString( const v8::Arguments& args ) {
        
        if (args.Length() != 0) {
            return v8::ThrowException( v8::String::New( "toString needs 0 arguments" ) );
        }
        
        v8::Handle<v8::Object> it = args.This();
        
        stringstream ss;
        long long val = numberLongVal( it );
        const long long limit = 2LL << 30;

        if ( val <= -limit || limit <= val )
            ss << "NumberLong(\"" << val << "\")";
        else
            ss << "NumberLong(" << val << ")";

        string ret = ss.str();
        return v8::String::New( ret.c_str() );
    }
    
    v8::Handle<v8::Value> bsonsize( const v8::Arguments& args ) {
        
        if (args.Length() != 1 || !args[ 0 ]->IsObject()) {
            return v8::ThrowException( v8::String::New( "bonsisze needs 1 object" ) );
        }

        return v8::Number::New( v8ToMongo( args[ 0 ]->ToObject() ).objsize() );
    }
}
