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
#include "engine_v8.h"
#include "v8_db.h"
#include "util/base64.h"
#include "util/text.h"
#include "../client/syncclusterconnection.h"
#include "../s/d_logic.h"
#include "../db/namespacestring.h"
#include <iostream>

using namespace std;
using namespace v8;

namespace mongo {

#define DDD(x)

    static v8::Handle<v8::Value> newInstance( v8::Function* f, const v8::Arguments& args ) {
        // need to translate arguments into an array
        int argc = args.Length();
        scoped_array< Handle<Value> > argv( new Handle<Value>[argc] );
        for (int i = 0; i < argc; ++i) {
            argv[i] = args[i];
        }
        return f->NewInstance(argc, argv.get());
    }

    v8::Handle<v8::FunctionTemplate> getMongoFunctionTemplate( V8Scope* scope, bool local ) {
        v8::Handle<v8::FunctionTemplate> mongo;
        if ( local ) {
            mongo = scope->createV8Function(mongoConsLocal);
        }
        else {
            mongo = scope->createV8Function(mongoConsExternal);
        }
        mongo->InstanceTemplate()->SetInternalFieldCount( 1 );
        v8::Handle<v8::Template> proto = mongo->PrototypeTemplate();
        scope->injectV8Function("find", mongoFind, proto);
        scope->injectV8Function("insert", mongoInsert, proto);
        scope->injectV8Function("remove", mongoRemove, proto);
        scope->injectV8Function("update", mongoUpdate, proto);
        scope->injectV8Function("auth", mongoAuth, proto);

        v8::Handle<FunctionTemplate> ic = scope->createV8Function(internalCursorCons);
        ic->InstanceTemplate()->SetInternalFieldCount( 1 );
        v8::Handle<v8::Template> icproto = ic->PrototypeTemplate();
        scope->injectV8Function("next", internalCursorNext, icproto);
        scope->injectV8Function("hasNext", internalCursorHasNext, icproto);
        scope->injectV8Function("objsLeftInBatch", internalCursorObjsLeftInBatch, icproto);
        scope->injectV8Function("readOnly", internalCursorReadOnly, icproto);
        proto->Set( scope->getV8Str( "internalCursor" ) , ic );

        return mongo;
    }

    v8::Handle<v8::FunctionTemplate> getNumberLongFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> numberLong = scope->createV8Function(numberLongInit);
        v8::Local<v8::Template> proto = numberLong->PrototypeTemplate();
        scope->injectV8Function("valueOf", numberLongValueOf, proto);
        scope->injectV8Function("toNumber", numberLongToNumber, proto);
        scope->injectV8Function("toString", numberLongToString, proto);

        return numberLong;
    }

    v8::Handle<v8::FunctionTemplate> getNumberIntFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> numberInt = scope->createV8Function(numberIntInit);
        v8::Local<v8::Template> proto = numberInt->PrototypeTemplate();
        scope->injectV8Function("valueOf", numberIntValueOf, proto);
        scope->injectV8Function("toNumber", numberIntToNumber, proto);
        scope->injectV8Function("toString", numberIntToString, proto);

        return numberInt;
    }

    v8::Handle<v8::FunctionTemplate> getBinDataFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> binData = scope->createV8Function(binDataInit);
        binData->InstanceTemplate()->SetInternalFieldCount(1);
        v8::Local<v8::Template> proto = binData->PrototypeTemplate();
        scope->injectV8Function("toString", binDataToString, proto);
        scope->injectV8Function("base64", binDataToBase64, proto);
        scope->injectV8Function("hex", binDataToHex, proto);
        return binData;
    }

    v8::Handle<v8::FunctionTemplate> getUUIDFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> templ = scope->createV8Function(uuidInit);
        return templ;
    }

    v8::Handle<v8::FunctionTemplate> getMD5FunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> templ = scope->createV8Function(md5Init);
        return templ;
    }

    v8::Handle<v8::FunctionTemplate> getHexDataFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> templ = scope->createV8Function(hexDataInit);
        return templ;
    }

    v8::Handle<v8::FunctionTemplate> getTimestampFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> ts = scope->createV8Function(dbTimestampInit);
        ts->InstanceTemplate()->SetInternalFieldCount( 1 );
        return ts;
    }

//    void installDBTypes( V8Scope* scope, Handle<ObjectTemplate>& global ) {
//        v8::Handle<v8::FunctionTemplate> db = scope->createV8Function(dbInit);
//        db->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );
//        global->Set(v8::String::New("DB") , db );
//
//        v8::Handle<v8::FunctionTemplate> dbCollection = scope->createV8Function(collectionInit);
//        dbCollection->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );
//        global->Set(v8::String::New("DBCollection") , dbCollection );
//
//
//        v8::Handle<v8::FunctionTemplate> dbQuery = scope->createV8Function(dbQueryInit);
//        dbQuery->InstanceTemplate()->SetIndexedPropertyHandler( dbQueryIndexAccess );
//        global->Set(v8::String::New("DBQuery") , dbQuery );
//
//        global->Set( v8::String::New("ObjectId") , newV8Function< objectIdInit >(scope) );
//
//        global->Set( v8::String::New("DBRef") , newV8Function< dbRefInit >(scope) );
//
//        global->Set( v8::String::New("DBPointer") , newV8Function< dbPointerInit >(scope) );
//
//        global->Set( v8::String::New("BinData") , getBinDataFunctionTemplate(scope) );
//
//        global->Set( v8::String::New("NumberLong") , getNumberLongFunctionTemplate(scope) );
//
//        global->Set( v8::String::New("Timestamp") , getTimestampFunctionTemplate(scope) );
//    }

    void installDBTypes( V8Scope* scope, v8::Handle<v8::Object>& global ) {
        v8::Handle<v8::FunctionTemplate> db = scope->createV8Function(dbInit);
        db->InstanceTemplate()->SetNamedPropertyHandler( collectionGetter, collectionSetter );
        global->Set(scope->getV8Str("DB") , db->GetFunction() );
        v8::Handle<v8::FunctionTemplate> dbCollection = scope->createV8Function(collectionInit);
        dbCollection->InstanceTemplate()->SetNamedPropertyHandler( collectionGetter, collectionSetter );
        global->Set(scope->getV8Str("DBCollection") , dbCollection->GetFunction() );


        v8::Handle<v8::FunctionTemplate> dbQuery = scope->createV8Function(dbQueryInit);
        dbQuery->InstanceTemplate()->SetIndexedPropertyHandler( dbQueryIndexAccess );
        global->Set(scope->getV8Str("DBQuery") , dbQuery->GetFunction() );

        scope->injectV8Function("ObjectId", objectIdInit, global);
        scope->injectV8Function("DBRef", dbRefInit, global);
        scope->injectV8Function("DBPointer", dbPointerInit, global);

        global->Set( scope->getV8Str("BinData") , getBinDataFunctionTemplate(scope)->GetFunction() );
        global->Set( scope->getV8Str("UUID") , getUUIDFunctionTemplate(scope)->GetFunction() );
        global->Set( scope->getV8Str("MD5") , getMD5FunctionTemplate(scope)->GetFunction() );
        global->Set( scope->getV8Str("HexData") , getHexDataFunctionTemplate(scope)->GetFunction() );
        global->Set( scope->getV8Str("NumberLong") , getNumberLongFunctionTemplate(scope)->GetFunction() );
        global->Set( scope->getV8Str("NumberInt") , getNumberIntFunctionTemplate(scope)->GetFunction() );
        global->Set( scope->getV8Str("Timestamp") , getTimestampFunctionTemplate(scope)->GetFunction() );

        BSONObjBuilder b;
        b.appendMaxKey( "" );
        b.appendMinKey( "" );
        BSONObj o = b.obj();
        BSONObjIterator i( o );
        global->Set( scope->getV8Str("MaxKey"), scope->mongoToV8Element( i.next() ) );
        global->Set( scope->getV8Str("MinKey"), scope->mongoToV8Element( i.next() ) );

        global->Get( scope->getV8Str( "Object" ) )->ToObject()->Set( scope->getV8Str("bsonsize") , scope->createV8Function(bsonsize)->GetFunction() );
    }

    void destroyConnection( Persistent<Value> self, void* parameter) {
        delete static_cast<DBClientBase*>(parameter);
        self.Dispose();
        self.Clear();
    }

    Handle<Value> mongoConsExternal(V8Scope* scope, const Arguments& args) {

        char host[255];

        if ( args.Length() > 0 && args[0]->IsString() ) {
            verify( args[0]->ToString()->Utf8Length() < 250 );
            args[0]->ToString()->WriteAscii( host );
        }
        else {
            strcpy( host , "127.0.0.1" );
        }

        string errmsg;
        ConnectionString cs = ConnectionString::parse( host , errmsg );
        if ( ! cs.isValid() )
            return v8::ThrowException( v8::String::New( errmsg.c_str() ) );


        DBClientWithCommands * conn;
        {
            //V8Unlock ul;
            conn = cs.connect( errmsg );
        }
        if ( ! conn )
            return v8::ThrowException( v8::String::New( errmsg.c_str() ) );

        Persistent<v8::Object> self = Persistent<v8::Object>::New( args.Holder() );
        self.MakeWeak( conn , destroyConnection );

        {
            //V8Unlock ul;
            ScriptEngine::runConnectCallback( *conn );
        }

        args.This()->SetInternalField( 0 , External::New( conn ) );
        args.This()->Set( scope->getV8Str( "slaveOk" ) , Boolean::New( false ) );
        args.This()->Set( scope->getV8Str( "host" ) , scope->getV8Str( host ) );

        return v8::Undefined();
    }

    Handle<Value> mongoConsLocal(V8Scope* scope, const Arguments& args) {

        if ( args.Length() > 0 )
            return v8::ThrowException( v8::String::New( "local Mongo constructor takes no args" ) );

        DBClientBase * conn;
        {
            //V8Unlock ul;
            conn = createDirectClient();
        }

        Persistent<v8::Object> self = Persistent<v8::Object>::New( args.This() );
        self.MakeWeak( conn , destroyConnection );

        // NOTE I don't believe the conn object will ever be freed.
        args.This()->SetInternalField( 0 , External::New( conn ) );
        args.This()->Set( scope->getV8Str( "slaveOk" ) , Boolean::New( false ) );
        args.This()->Set( scope->getV8Str( "host" ) , scope->getV8Str( "EMBEDDED" ) );

        return v8::Undefined();
    }


    // ---

#ifdef _WIN32
#define GETNS char * ns = new char[args[0]->ToString()->Utf8Length()];  args[0]->ToString()->WriteUtf8( ns );
#else
#define GETNS char ns[args[0]->ToString()->Utf8Length()];  args[0]->ToString()->WriteUtf8( ns );
#endif

    DBClientBase * getConnection( const Arguments& args ) {
        Local<External> c = External::Cast( *(args.This()->GetInternalField( 0 )) );
        DBClientBase * conn = (DBClientBase*)(c->Value());
        verify( conn );
        return conn;
    }

    // ---- real methods

    void destroyCursor( Persistent<Value> self, void* parameter) {
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
    Handle<Value> mongoFind(V8Scope* scope, const Arguments& args) {
        HandleScope handle_scope;

        jsassert( args.Length() == 7 , "find needs 7 args" );
        jsassert( args[1]->IsObject() , "needs to be an object" );
        DBClientBase * conn = getConnection( args );
        GETNS;

        BSONObj q = scope->v8ToMongo( args[1]->ToObject() );
        DDD( "query:" << q  );

        BSONObj fields;
        bool haveFields = args[2]->IsObject() && args[2]->ToObject()->GetPropertyNames()->Length() > 0;
        if ( haveFields )
            fields = scope->v8ToMongo( args[2]->ToObject() );

        Local<v8::Object> mongo = args.This();

        try {
            auto_ptr<mongo::DBClientCursor> cursor;
            int nToReturn = (int)(args[3]->ToNumber()->Value());
            int nToSkip = (int)(args[4]->ToNumber()->Value());
            int batchSize = (int)(args[5]->ToNumber()->Value());
            int options = (int)(args[6]->ToNumber()->Value());
            {
                //V8Unlock u;
                cursor = conn->query( ns, q ,  nToReturn , nToSkip , haveFields ? &fields : 0, options , batchSize );
                if ( ! cursor.get() ) 
                    return v8::ThrowException( v8::String::New( "error doing query: failed" ) );
            }
            v8::Function * cons = (v8::Function*)( *( mongo->Get( scope->getV8Str( "internalCursor" ) ) ) );
            if ( !cons ) {
                // may get here in case of thread termination
                return v8::ThrowException( v8::String::New( "Could not create a cursor" ) );
            }

            Persistent<v8::Object> c = Persistent<v8::Object>::New( cons->NewInstance() );
            c.MakeWeak( cursor.get() , destroyCursor );
            c->SetInternalField( 0 , External::New( cursor.release() ) );
            return handle_scope.Close(c);
        }
        catch ( ... ) {
            return v8::ThrowException( v8::String::New( "socket error on query" ) );
        }
    }

    v8::Handle<v8::Value> mongoInsert(V8Scope* scope, const v8::Arguments& args) {
        jsassert( args.Length() == 2 , "insert needs 2 args" );
        jsassert( args[1]->IsObject() , "have to insert an object" );

        if ( args.This()->Get( scope->getV8Str( "readOnly" ) )->BooleanValue() )
            return v8::ThrowException( v8::String::New( "js db in read only mode" ) );

        DBClientBase * conn = getConnection( args );
        GETNS;

        v8::Handle<v8::Object> in = args[1]->ToObject();

        if( args[1]->IsArray() ){

            v8::Local<v8::Array> arr = v8::Array::Cast( *args[1] );
            vector<BSONObj> bos;
            uint32_t len = arr->Length();

            for( uint32_t i = 0; i < len; i++ ){

                v8::Local<v8::Object> el = arr->CloneElementAt( i );

                // Set ID on the element if necessary
                if ( ! el->Has( scope->getV8Str( "_id" ) ) ) {
                    v8::Handle<v8::Value> argv[1];
                    el->Set( scope->getV8Str( "_id" ) , scope->getObjectIdCons()->NewInstance( 0 , argv ) );
                }

                bos.push_back( scope->v8ToMongo( arr->CloneElementAt( i ) ) );
            }

            DDD( "want to save batch : " << bos.length );
            try {
                //V8Unlock u;
                conn->insert( ns , bos );
            }
            catch ( ... ) {
                return v8::ThrowException( v8::String::New( "socket error on bulk insert" ) );
            }

        }
        else {

            if ( ! in->Has( scope->getV8Str( "_id" ) ) ) {
                v8::Handle<v8::Value> argv[1];
                in->Set( scope->getV8Str( "_id" ) , scope->getObjectIdCons()->NewInstance( 0 , argv ) );
            }

            BSONObj o = scope->v8ToMongo( in );

            DDD( "want to save : " << o.jsonString() );
            try {
                //V8Unlock u;
                conn->insert( ns , o );
            }
            catch ( ... ) {
                return v8::ThrowException( v8::String::New( "socket error on insert" ) );
            }

        }

        return v8::Undefined();
    }

    v8::Handle<v8::Value> mongoRemove(V8Scope* scope, const v8::Arguments& args) {
        jsassert( args.Length() == 2 || args.Length() == 3 , "remove needs 2 args" );
        jsassert( args[1]->IsObject() , "have to remove an object template" );

        if ( args.This()->Get( scope->getV8Str( "readOnly" ) )->BooleanValue() )
            return v8::ThrowException( v8::String::New( "js db in read only mode" ) );

        DBClientBase * conn = getConnection( args );
        GETNS;

        v8::Handle<v8::Object> in = args[1]->ToObject();
        BSONObj o = scope->v8ToMongo( in );

        bool justOne = false;
        if ( args.Length() > 2 ) {
            justOne = args[2]->BooleanValue();
        }

        DDD( "want to remove : " << o.jsonString() );
        try {
            //V8Unlock u;
            conn->remove( ns , o , justOne );
        }
        catch ( ... ) {
            return v8::ThrowException( v8::String::New( "socket error on remove" ) );
        }

        return v8::Undefined();
    }

    v8::Handle<v8::Value> mongoUpdate(V8Scope* scope, const v8::Arguments& args) {
        jsassert( args.Length() >= 3 , "update needs at least 3 args" );
        jsassert( args[1]->IsObject() , "1st param to update has to be an object" );
        jsassert( args[2]->IsObject() , "2nd param to update has to be an object" );
        
        if ( args.This()->Get( scope->getV8Str( "readOnly" ) )->BooleanValue() )
            return v8::ThrowException( v8::String::New( "js db in read only mode" ) );

        DBClientBase * conn = getConnection( args );
        GETNS;

        v8::Handle<v8::Object> q = args[1]->ToObject();
        v8::Handle<v8::Object> o = args[2]->ToObject();

        bool upsert = args.Length() > 3 && args[3]->IsBoolean() && args[3]->ToBoolean()->Value();
        bool multi = args.Length() > 4 && args[4]->IsBoolean() && args[4]->ToBoolean()->Value();

        try {
            BSONObj q1 = scope->v8ToMongo( q );
            BSONObj o1 = scope->v8ToMongo( o );
            //V8Unlock u;
            conn->update( ns , q1 , o1 , upsert, multi );
        }
        catch ( ... ) {
            return v8::ThrowException( v8::String::New( "socket error on remove" ) );
        }

        return v8::Undefined();
    }

    v8::Handle<v8::Value> mongoAuth(V8Scope* scope, const v8::Arguments& args) {
        jsassert( args.Length() >= 3 , "update needs at least 3 args" );
        DBClientBase * conn = getConnection( args );
        string db = toSTLString(args[0]);
        string username = toSTLString(args[1]);
        string password = toSTLString(args[2]);
        string errmsg = "";

        try {
            if (conn->auth(db, username, password, errmsg)) {
                return v8::Boolean::New(true);
            }
        } catch ( ... ) {
        }
        return v8::ThrowException( v8::String::New( errmsg.c_str() ) );
    }

//    +    JSBool mongo_auth(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval) {
//    +        smuassert( cx , "mongo_auth needs 3 args" , argc == 3 );
//    +        shared_ptr< DBClientWithCommands > * connHolder = (shared_ptr< DBClientWithCommands >*)JS_GetPrivate( cx , obj );
//    +        smuassert( cx ,  "no connection!" , connHolder && connHolder->get() );
//    +        DBClientWithCommands *conn = connHolder->get();
//    +
//    +        Convertor c( cx );
//    +
//    +        string db = c.toString( argv[0] );
//    +        string username = c.toString( argv[1] );
//    +        string password = c.toString( argv[2] );
//    +        string errmsg = "";
//    +
//    +        try {
//    +            if (conn->auth(db, username, password, errmsg)) {
//    +                return JS_TRUE;
//    +            }
//    +            JS_ReportError( cx, errmsg.c_str() );
//    +        }
//    +        catch ( ... ) {
//    +            JS_ReportError( cx , "error doing query: unknown" );
//    +        }
//    +        return JS_FALSE;
//    +    }


    // --- cursor ---

    mongo::DBClientCursor * getCursor( const Arguments& args ) {
        Local<External> c = External::Cast( *(args.This()->GetInternalField( 0 ) ) );

        mongo::DBClientCursor * cursor = (mongo::DBClientCursor*)(c->Value());
        return cursor;
    }

    v8::Handle<v8::Value> internalCursorCons(V8Scope* scope, const v8::Arguments& args) {
        return v8::Undefined();
    }

    v8::Handle<v8::Value> internalCursorNext(V8Scope* scope, const v8::Arguments& args) {
        mongo::DBClientCursor * cursor = getCursor( args );
        if ( ! cursor )
            return v8::Undefined();
        BSONObj o;
        {
            //V8Unlock u;
            o = cursor->next();
        }
        bool ro = false;
        if (args.This()->Has(scope->V8STR_RO))
            ro = args.This()->Get(scope->V8STR_RO)->BooleanValue();
        return scope->mongoToLZV8( o, false, ro );
    }

    v8::Handle<v8::Value> internalCursorHasNext(V8Scope* scope, const v8::Arguments& args) {
        mongo::DBClientCursor * cursor = getCursor( args );
        if ( ! cursor )
            return Boolean::New( false );
        bool ret;
        {
            //V8Unlock u;
            ret = cursor->more();
        }
        return Boolean::New( ret );
    }

    v8::Handle<v8::Value> internalCursorObjsLeftInBatch(V8Scope* scope, const v8::Arguments& args) {
        mongo::DBClientCursor * cursor = getCursor( args );
        if ( ! cursor )
            return v8::Number::New( (double) 0 );
        int ret;
        {
            //V8Unlock u;
            ret = cursor->objsLeftInBatch();
        }
        return v8::Number::New( (double) ret );
    }

    v8::Handle<v8::Value> internalCursorReadOnly(V8Scope* scope, const v8::Arguments& args) {
        Local<v8::Object> cursor = args.This();
        cursor->Set(scope->V8STR_RO, v8::Boolean::New(true));
        return cursor;
    }

    // --- DB ----

    v8::Handle<v8::Value> dbInit(V8Scope* scope, const v8::Arguments& args) {
        verify( args.Length() == 2 );

        args.This()->Set( scope->getV8Str( "_mongo" ) , args[0] );
        args.This()->Set( scope->getV8Str( "_name" ) , args[1] );

        for ( int i=0; i<args.Length(); i++ )
            verify( ! args[i]->IsUndefined() );

        string dbName = toSTLString( args[1] );
        if ( !NamespaceString::validDBName( dbName)) {
            string msg = str::stream() << "[" << dbName << "] is not a "
                                       << "valid database name";
            return v8::ThrowException( v8::String::New( msg.c_str() ));
        }
        
        return v8::Undefined();
    }

    v8::Handle<v8::Value> collectionInit( V8Scope* scope, const v8::Arguments& args ) {
        verify( args.Length() == 4 );

        args.This()->Set( scope->getV8Str( "_mongo" ) , args[0] );
        args.This()->Set( scope->getV8Str( "_db" ) , args[1] );
        args.This()->Set( scope->getV8Str( "_shortName" ) , args[2] );
        args.This()->Set( scope->V8STR_FULLNAME , args[3] );
        
        if ( haveLocalShardingInfo( toSTLString( args[3] ) ) )
            return v8::ThrowException( v8::String::New( "can't use sharded collection from db.eval" ) );

        for ( int i=0; i<args.Length(); i++ )
            verify( ! args[i]->IsUndefined() );

        return v8::Undefined();
    }

    v8::Handle<v8::Value> dbQueryInit( V8Scope* scope, const v8::Arguments& args ) {

        v8::Handle<v8::Object> t = args.This();

        verify( args.Length() >= 4 );

        t->Set( scope->getV8Str( "_mongo" ) , args[0] );
        t->Set( scope->getV8Str( "_db" ) , args[1] );
        t->Set( scope->getV8Str( "_collection" ) , args[2] );
        t->Set( scope->getV8Str( "_ns" ) , args[3] );

        if ( args.Length() > 4 && args[4]->IsObject() )
            t->Set( scope->getV8Str( "_query" ) , args[4] );
        else
            t->Set( scope->getV8Str( "_query" ) , v8::Object::New() );

        if ( args.Length() > 5 && args[5]->IsObject() )
            t->Set( scope->getV8Str( "_fields" ) , args[5] );
        else
            t->Set( scope->getV8Str( "_fields" ) , v8::Null() );


        if ( args.Length() > 6 && args[6]->IsNumber() )
            t->Set( scope->getV8Str( "_limit" ) , args[6] );
        else
            t->Set( scope->getV8Str( "_limit" ) , Number::New( 0 ) );

        if ( args.Length() > 7 && args[7]->IsNumber() )
            t->Set( scope->getV8Str( "_skip" ) , args[7] );
        else
            t->Set( scope->getV8Str( "_skip" ) , Number::New( 0 ) );

        if ( args.Length() > 8 && args[8]->IsNumber() )
            t->Set( scope->getV8Str( "_batchSize" ) , args[8] );
        else
            t->Set( scope->getV8Str( "_batchSize" ) , Number::New( 0 ) );

        if ( args.Length() > 9 && args[9]->IsNumber() )
            t->Set( scope->getV8Str( "_options" ) , args[9] );
        else
            t->Set( scope->getV8Str( "_options" ) , Number::New( 0 ) );

        
        t->Set( scope->getV8Str( "_cursor" ) , v8::Null() );
        t->Set( scope->getV8Str( "_numReturned" ) , v8::Number::New(0) );
        t->Set( scope->getV8Str( "_special" ) , Boolean::New(false) );

        return v8::Undefined();
    }

    Handle<Value> collectionSetter( Local<v8::String> name, Local<Value> value, const AccessorInfo& info ) {
        // a collection name cannot be overwritten by a variable
        string sname = toSTLString( name );
        if ( sname.length() == 0 || sname[0] == '_' ) {
            // if starts with '_' we allow overwrite
            return Handle<Value>();
        }
        // dont set
        return value;
    }

    v8::Handle<v8::Value> collectionGetter( v8::Local<v8::String> name, const v8::AccessorInfo &info) {
        DDD( "collectionFallback [" << name << "]" );

        // first look in prototype, may be a function
        v8::Handle<v8::Value> real = info.This()->GetPrototype()->ToObject()->Get( name );
        if ( !real->IsUndefined() )
            return real;

        // 2nd look into real values, may be cached collection object
        string sname = toSTLString( name );
        if (info.This()->HasRealNamedProperty(name)) {
            v8::Local<v8::Value> prop = info.This()->GetRealNamedProperty( name );
            if (prop->IsObject() && prop->ToObject()->HasRealNamedProperty(v8::String::New("_fullName"))) {
                // need to check every time that the collection did not get sharded
                if ( haveLocalShardingInfo( toSTLString( prop->ToObject()->GetRealNamedProperty(v8::String::New("_fullName")) ) ) )
                    return v8::ThrowException( v8::String::New( "can't use sharded collection from db.eval" ) );
            }
            return prop;
        } else if ( sname.length() == 0 || sname[0] == '_' ) {
            // if starts with '_' we dont return collection, one must use getCollection()
            return v8::Handle<v8::Value>();
        }

        // no hit, create new collection
        v8::Handle<v8::Value> getCollection = info.This()->GetPrototype()->ToObject()->Get( v8::String::New( "getCollection" ) );
        verify( getCollection->IsFunction() );

        TryCatch tryCatch;
        v8::Function * f = (v8::Function*)(*getCollection);
        v8::Handle<v8::Value> argv[1];
        argv[0] = name;
        v8::Local<v8::Value> coll = f->Call( info.This() , 1 , argv );
        if (coll.IsEmpty()) {
            if (tryCatch.HasCaught()) {
                return v8::ThrowException( tryCatch.Exception() );
            }
            return Handle<Value>();
        }

        // cache collection for reuse, dont enumerate
        info.This()->ForceSet(name, coll, v8::DontEnum);
        return coll;
    }

    v8::Handle<v8::Value> dbQueryIndexAccess( unsigned int index , const v8::AccessorInfo& info ) {
        v8::Handle<v8::Value> arrayAccess = info.This()->GetPrototype()->ToObject()->Get( v8::String::New( "arrayAccess" ) );
        verify( arrayAccess->IsFunction() );

        v8::Function * f = (v8::Function*)(*arrayAccess);
        v8::Handle<v8::Value> argv[1];
        argv[0] = v8::Number::New( index );

        return f->Call( info.This() , 1 , argv );
    }

    v8::Handle<v8::Value> objectIdInit( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ) {
            v8::Function * f = scope->getObjectIdCons();
            return newInstance(f, args);
        }

        OID oid;

        if ( args.Length() == 0 ) {
            oid.init();
        }
        else {
            string s = toSTLString( args[0] );
            try {
                Scope::validateObjectIdString( s );
            }
            catch ( const MsgAssertionException &m ) {
                string error = m.toString();
                return v8::ThrowException( v8::String::New( error.c_str() ) );
            }
            oid.init( s );
        }

        it->Set( scope->getV8Str( "str" ) , v8::String::New( oid.str().c_str() ) );

        return it;
    }

    v8::Handle<v8::Value> dbRefInit( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ) {
            v8::Function * f = scope->getNamedCons( "DBRef" );
            return newInstance(f, args);
        }

        if (args.Length() != 2 && args.Length() != 0) {
            return v8::ThrowException( v8::String::New( "DBRef needs 2 arguments" ) );
        }

        if ( args.Length() == 2 ) {
            it->Set( scope->getV8Str( "$ref" ) , args[0] );
            it->Set( scope->getV8Str( "$id" ) , args[1] );
        }

        return it;
    }

    v8::Handle<v8::Value> dbPointerInit( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ) {
            v8::Function * f = scope->getNamedCons( "DBPointer" );
            return newInstance(f, args);
        }

        if (args.Length() != 2) {
            return v8::ThrowException( v8::String::New( "DBPointer needs 2 arguments" ) );
        }

        it->Set( scope->getV8Str( "ns" ) , args[0] );
        it->Set( scope->getV8Str( "id" ) , args[1] );
        it->SetHiddenValue( scope->getV8Str( "__DBPointer" ), v8::Number::New( 1 ) );

        return it;
    }

    v8::Handle<v8::Value> dbTimestampInit( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ) {
            v8::Function * f = scope->getNamedCons( "Timestamp" );
            return newInstance(f, args);
        }

        if ( args.Length() == 0 ) {
            it->Set( scope->getV8Str( "t" ) , v8::Number::New( 0 ) );
            it->Set( scope->getV8Str( "i" ) , v8::Number::New( 0 ) );
        }
        else if ( args.Length() == 2 ) {
            it->Set( scope->getV8Str( "t" ) , args[0] );
            it->Set( scope->getV8Str( "i" ) , args[1] );
        }
        else {
            return v8::ThrowException( v8::String::New( "Timestamp needs 0 or 2 arguments" ) );
        }

        it->SetInternalField( 0, v8::Uint32::New( Timestamp ) );

        return it;
    }


    v8::Handle<v8::Value> binDataInit( V8Scope* scope, const v8::Arguments& args ) {
        v8::Local<v8::Object> it = args.This();
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ) {
            v8::Function* f = scope->getNamedCons( "BinData" );
            return newInstance(f, args);
        }

        Handle<Value> type;
        Handle<Value> len;
        int rlen;
        char* data;
        if (args.Length() == 3) {
            // 3 args: len, type, data
            len = args[0];
            rlen = len->IntegerValue();
            type = args[1];
            v8::String::Utf8Value utf( args[ 2 ] );
            char* tmp = *utf;
            data = new char[rlen];
            memcpy(data, tmp, rlen);
        }
        else if ( args.Length() == 2 ) {
            // 2 args: type, base64 string
            type = args[0];
            v8::String::Utf8Value utf( args[ 1 ] );
            string decoded = base64::decode( *utf );
            const char* tmp = decoded.data();
            rlen = decoded.length();
            data = new char[rlen];
            memcpy(data, tmp, rlen);
            len = v8::Number::New(rlen);
//            it->Set( scope->getV8Str( "data" ), v8::String::New( decoded.data(), decoded.length() ) );
        } else if (args.Length() == 0) {
            // this is called by subclasses that will fill properties
            return it;
        } else {
            return v8::ThrowException( v8::String::New( "BinData needs 2 or 3 arguments" ) );
        }

        it->Set( scope->getV8Str( "len" ) , len );
        it->Set( scope->getV8Str( "type" ) , type );
        it->SetHiddenValue( scope->V8STR_BINDATA, v8::Number::New( 1 ) );
        Persistent<v8::Object> res = scope->wrapArrayObject(it, data);
        return res;
    }

    v8::Handle<v8::Value> binDataToString( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        int len = it->Get( scope->V8STR_LEN )->Int32Value();
        int type = it->Get( scope->V8STR_TYPE )->Int32Value();
        Local<External> c = External::Cast( *(it->GetInternalField( 0 )) );
        char* data = (char*)(c->Value());

        stringstream ss;
        ss << "BinData(" << type << ",\"";
        base64::encode( ss, data, len );
        ss << "\")";
        string ret = ss.str();
        return v8::String::New( ret.c_str() );
    }

    v8::Handle<v8::Value> binDataToBase64( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        int len = Handle<v8::Number>::Cast(it->Get(scope->V8STR_LEN))->Int32Value();
        Local<External> c = External::Cast( *(it->GetInternalField( 0 )) );
        char* data = (char*)(c->Value());
        stringstream ss;
        base64::encode( ss, (const char *)data, len );
        return v8::String::New(ss.str().c_str());
    }

    v8::Handle<v8::Value> binDataToHex( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        int len = Handle<v8::Number>::Cast(it->Get(scope->V8STR_LEN))->Int32Value();
        Local<External> c = External::Cast( *(it->GetInternalField( 0 )) );
        char* data = (char*)(c->Value());
        stringstream ss;
        ss.setf (ios_base::hex , ios_base::basefield);
        ss.fill ('0');
        ss.setf (ios_base::right , ios_base::adjustfield);
        for( int i = 0; i < len; i++ ) {
            unsigned v = (unsigned char) data[i];
            ss << setw(2) << v;
        }
        return v8::String::New(ss.str().c_str());
    }

    static v8::Handle<v8::Value> hexToBinData( V8Scope* scope, v8::Local<v8::Object> it, int type, string hexstr ) {
        int len = hexstr.length() / 2;
        char* data = new char[len];
        const char* src = hexstr.c_str();
        for( int i = 0; i < 16; i++ ) {
            data[i] = fromHex(src + i * 2);
        }

        it->Set( scope->V8STR_LEN , v8::Number::New(len) );
        it->Set( scope->V8STR_TYPE , v8::Number::New(type) );
        it->SetHiddenValue( scope->V8STR_BINDATA, v8::Number::New( 1 ) );
        Persistent<v8::Object> res = scope->wrapArrayObject(it, data);
        return res;
    }

    v8::Handle<v8::Value> uuidInit( V8Scope* scope, const v8::Arguments& args ) {
        if (args.Length() != 1) {
            return v8::ThrowException( v8::String::New( "UUIS needs 1 argument" ) );
        }
        v8::String::Utf8Value utf( args[ 0 ] );
        if( utf.length() != 32 ) {
            return v8::ThrowException( v8::String::New( "UUIS string must have 32 characters" ) );
        }

        v8::Function * f = scope->getNamedCons("BinData");
        Local<v8::Object> it = f->NewInstance();
        return hexToBinData(scope, it, bdtUUID, *utf);
    }

    v8::Handle<v8::Value> md5Init( V8Scope* scope, const v8::Arguments& args ) {
        if (args.Length() != 1) {
            return v8::ThrowException( v8::String::New( "MD5 needs 1 argument" ) );
        }
        v8::String::Utf8Value utf( args[ 0 ] );
        if( utf.length() != 32 ) {
            return v8::ThrowException( v8::String::New( "MD5 string must have 32 characters" ) );
        }

        v8::Function * f = scope->getNamedCons("BinData");
        Local<v8::Object> it = f->NewInstance();
        return hexToBinData(scope, it, MD5Type, *utf);
    }

    v8::Handle<v8::Value> hexDataInit( V8Scope* scope, const v8::Arguments& args ) {
        if (args.Length() != 2) {
            return v8::ThrowException( v8::String::New( "HexData needs 2 arguments" ) );
        }
        v8::String::Utf8Value utf( args[ 1 ] );
        v8::Function * f = scope->getNamedCons("BinData");
        Local<v8::Object> it = f->NewInstance();
        return hexToBinData(scope, it, args[0]->IntegerValue(), *utf);
    }

    v8::Handle<v8::Value> numberLongInit( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ) {
            v8::Function * f = scope->getNamedCons( "NumberLong" );
            return newInstance(f, args);
        }

        if (args.Length() != 0 && args.Length() != 1 && args.Length() != 3) {
            return v8::ThrowException( v8::String::New( "NumberLong needs 0, 1 or 3 arguments" ) );
        }

        if ( args.Length() == 0 ) {
            it->Set( scope->getV8Str( "floatApprox" ), v8::Number::New( 0 ) );
        }
        else if ( args.Length() == 1 ) {
            if ( args[ 0 ]->IsNumber() ) {
                it->Set( scope->getV8Str( "floatApprox" ), args[ 0 ] );
            }
            else {
                v8::String::Utf8Value data( args[ 0 ] );
                string num = *data;
                const char *numStr = num.c_str();
                long long n;
                try {
                    n = parseLL( numStr );
                }
                catch ( const AssertionException & ) {
                    return v8::ThrowException( v8::String::New( "could not convert string to long long" ) );
                }
                unsigned long long val = n;
                // values above 2^53 are not accurately represented in JS
                if ( (long long)val == (long long)(double)(long long)(val) && val < 9007199254740992ULL ) {
                    it->Set( scope->getV8Str( "floatApprox" ), v8::Number::New( (double)(long long)( val ) ) );
                }
                else {
                    it->Set( scope->getV8Str( "floatApprox" ), v8::Number::New( (double)(long long)( val ) ) );
                    it->Set( scope->getV8Str( "top" ), v8::Integer::New( val >> 32 ) );
                    it->Set( scope->getV8Str( "bottom" ), v8::Integer::New( (unsigned long)(val & 0x00000000ffffffff) ) );
                }
            }
        }
        else {
            it->Set( scope->getV8Str( "floatApprox" ) , args[0] );
            it->Set( scope->getV8Str( "top" ) , args[1] );
            it->Set( scope->getV8Str( "bottom" ) , args[2] );
        }
        it->SetHiddenValue( scope->V8STR_NUMBERLONG, v8::Number::New( 1 ) );

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

    v8::Handle<v8::Value> numberLongValueOf( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        long long val = numberLongVal( it );
        return v8::Number::New( double( val ) );
    }

    v8::Handle<v8::Value> numberLongToNumber( V8Scope* scope, const v8::Arguments& args ) {
        return numberLongValueOf( scope, args );
    }

    v8::Handle<v8::Value> numberLongToString( V8Scope* scope, const v8::Arguments& args ) {
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

    v8::Handle<v8::Value> numberIntInit( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        if ( it->IsUndefined() || it == v8::Context::GetCurrent()->Global() ) {
            v8::Function * f = scope->getNamedCons( "NumberInt" );
            return newInstance(f, args);
        }

        if (args.Length() != 0 && args.Length() != 1) {
            return v8::ThrowException( v8::String::New( "NumberInt needs 0, 1 argument" ) );
        }

        if ( args.Length() == 0 ) {
            it->SetHiddenValue( scope->V8STR_NUMBERINT, v8::Number::New( 0 ) );
        }
        else if ( args.Length() == 1 ) {
            it->SetHiddenValue( scope->V8STR_NUMBERINT, args[0]->ToInt32() );
        }

        return it;
    }

    v8::Handle<v8::Value> numberIntValueOf( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();
        int val = it->GetHiddenValue( scope->V8STR_NUMBERINT )->Int32Value();
        return v8::Number::New( double( val ) );
    }

    v8::Handle<v8::Value> numberIntToNumber( V8Scope* scope, const v8::Arguments& args ) {
        return numberIntValueOf( scope, args );
    }

    v8::Handle<v8::Value> numberIntToString( V8Scope* scope, const v8::Arguments& args ) {
        v8::Handle<v8::Object> it = args.This();

        stringstream ss;
        int val = it->GetHiddenValue( scope->V8STR_NUMBERINT )->Int32Value();
        ss << "NumberInt(" << val << ")";

        string ret = ss.str();
        return v8::String::New( ret.c_str() );
    }

    v8::Handle<v8::Value> bsonsize( V8Scope* scope, const v8::Arguments& args ) {

        if ( args.Length() != 1 )
            return v8::ThrowException( v8::String::New( "bsonsize needs 1 argument" ) );

        if ( args[0]->IsNull() )
            return v8::Number::New(0);

        if ( ! args[ 0 ]->IsObject() )
            return v8::ThrowException( v8::String::New( "argument to bsonsize has to be an object" ) );

        return v8::Number::New( scope->v8ToMongo( args[ 0 ]->ToObject() ).objsize() );
    }

    namespace v8Locks {
        boost::mutex& __interruptMutex = *( new boost::mutex );

        InterruptLock::InterruptLock() {
            __interruptMutex.lock();
        }

        InterruptLock::~InterruptLock() {
            __interruptMutex.unlock();
        }

        boost::mutex& __v8Mutex = *( new boost::mutex );
        ThreadLocalValue< bool > __locked;

        RecursiveLock::RecursiveLock() : _unlock() {
            if ( !__locked.get() ) {
                __v8Mutex.lock();
                __locked.set( true );
                _unlock = true;
            }
        }
        RecursiveLock::~RecursiveLock() {
            if ( _unlock ) {
                __v8Mutex.unlock();
                __locked.set( false );
            }
        }

        RecursiveUnlock::RecursiveUnlock() : _lock() {
            if ( __locked.get() ) {
                __v8Mutex.unlock();
                __locked.set( false );
                _lock = true;
            }
        }
        RecursiveUnlock::~RecursiveUnlock() {
            if ( _lock ) {
                __v8Mutex.lock();
                __locked.set( true );
            }
        }
    } // namespace v8Locks
}
