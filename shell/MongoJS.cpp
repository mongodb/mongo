// MongoJS.cpp

#include "MongoJS.h"
#include "ShellUtils.h"

#include <iostream>

using namespace std;
using namespace mongo;
using namespace v8;

#define CONN_STRING (String::New( "_conn" ))

#define DDD(x)
//#define DDD(x) ( cout << x << endl );

void installMongoGlobals( Handle<ObjectTemplate>& global ){
    global->Set(String::New("mongoInject"), FunctionTemplate::New(mongoInject));

    v8::Local<v8::FunctionTemplate> mongo = FunctionTemplate::New( mongoInit );
    global->Set(String::New("Mongo") , mongo );
    
    v8::Local<v8::FunctionTemplate> db = FunctionTemplate::New( dbInit );
    global->Set(String::New("DB") , db );
    db->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );

    v8::Local<v8::FunctionTemplate> dbCollection = FunctionTemplate::New( collectionInit );
    global->Set(String::New("DBCollection") , dbCollection );
    dbCollection->InstanceTemplate()->SetNamedPropertyHandler( collectionFallback );

    v8::Local<v8::FunctionTemplate> dbQuery = FunctionTemplate::New( dbQueryInit );
    global->Set(String::New("DBQuery") , dbQuery );
    dbQuery->InstanceTemplate()->SetIndexedPropertyHandler( dbQueryIndexAccess );
    
    v8::Local<v8::FunctionTemplate> objectId = FunctionTemplate::New( objectIdInit );
    global->Set(String::New("ObjectId") , objectId );
}

Handle<Value> mongoInject(const Arguments& args){
    jsassert( args.Length() == 1 , "mongoInject takes exactly 1 argument" );
    jsassert( args[0]->IsObject() , "mongoInject needs to be passed a prototype" );

    Local<v8::Object> o = args[0]->ToObject();
    
    o->Set( String::New( "init" ) , FunctionTemplate::New( mongoInit )->GetFunction() );
    o->Set( String::New( "find" ) , FunctionTemplate::New( mongoFind )->GetFunction() );
    o->Set( String::New( "insert" ) , FunctionTemplate::New( mongoInsert )->GetFunction() );
    o->Set( String::New( "remove" ) , FunctionTemplate::New( mongoRemove )->GetFunction() );
    o->Set( String::New( "update" ) , FunctionTemplate::New( mongoUpdate )->GetFunction() );
    
    Local<FunctionTemplate> t = FunctionTemplate::New( internalCursorCons );
    t->PrototypeTemplate()->Set( String::New("next") , FunctionTemplate::New( internalCursorNext ) );
    t->PrototypeTemplate()->Set( String::New("hasNext") , FunctionTemplate::New( internalCursorHasNext ) );
    o->Set( String::New( "internalCursor" ) , t->GetFunction() );
    
    return v8::Undefined();
}

Handle<Value> mongoInit(const Arguments& args){

    char host[255];
    
    if ( args.Length() > 0 && args[0]->IsString() ){
        assert( args[0]->ToString()->Utf8Length() < 250 );
        args[0]->ToString()->WriteAscii( host );
    }
    else {
        strcpy( host , "127.0.0.1" );
    }

    DBClientConnection * conn = new DBClientConnection( true );

    string errmsg;
    if ( ! conn->connect( host , errmsg ) ){
        return v8::ThrowException( v8::String::New( "couldn't connect" ) );
    }

    // NOTE I don't believe the conn object will ever be freed.
    args.This()->Set( CONN_STRING , External::New( conn ) );
    args.This()->Set( String::New( "slaveOk" ) , Boolean::New( false ) );
    
    return v8::Undefined();
}


// ---

Local<v8::Object> mongoToV8( BSONObj & m , bool array ){
    Local<v8::Object> o;
    if ( array )
        o = v8::Array::New();
    else 
        o = v8::Object::New();
    
    mongo::BSONObj sub;
    
    for ( BSONObjIterator i(m); i.more(); ) {
        BSONElement f = i.next();
        if ( f.eoo() )
            break;
        
        Local<Value> v;
        
        switch ( f.type() ){

        case mongo::Code:
            cout << "warning, code saved in database just turned into string right now" << endl;
        case mongo::String: 
            o->Set( v8::String::New( f.fieldName() ) , v8::String::New( f.valuestr() ) );
            break;
            
        case mongo::jstOID: {
            v8::Function * idCons = getObjectIdCons();
            v8::Handle<v8::Value> argv[1];
            argv[0] = v8::String::New( f.__oid().str().c_str() );
            o->Set( v8::String::New( f.fieldName() ) , 
                    idCons->NewInstance( 1 , argv ) );
            break;
        }
            
        case mongo::NumberDouble:
        case mongo::NumberInt:
            o->Set( v8::String::New( f.fieldName() ) , v8::Number::New( f.number() ) );
            break;
            
        case mongo::Array:
        case mongo::Object:
            sub = f.embeddedObject();
            o->Set( v8::String::New( f.fieldName() ) , mongoToV8( sub , f.type() == mongo::Array ) );
            break;
            
        case mongo::Date:
            o->Set( v8::String::New( f.fieldName() ) , v8::Date::New( f.date() ) );
            break;

        case mongo::Bool:
            o->Set( v8::String::New( f.fieldName() ) , v8::Boolean::New( f.boolean() ) );
            break;
            
        case mongo::jstNULL:
            o->Set( v8::String::New( f.fieldName() ) , v8::Null() );
            break;
            
        case mongo::RegEx: {
            v8::Function * regex = getNamedCons( "RegExp" );
            
            v8::Handle<v8::Value> argv[2];
            argv[0] = v8::String::New( f.regex() );
            argv[1] = v8::String::New( f.regexFlags() );
            
            o->Set( v8::String::New( f.fieldName() ) , regex->NewInstance( 2 , argv ) );
            break;
        }
            
        case mongo::BinData: {
            Local<v8::Object> b = v8::Object::New();

            int len;
            f.binData( len );
            
            b->Set( v8::String::New( "subtype" ) , v8::Number::New( f.binDataType() ) );
            b->Set( v8::String::New( "length" ) , v8::Number::New( len ) );
            
            o->Set( v8::String::New( f.fieldName() ) , b );
            break;
        };
            
        case mongo::Timestamp: {
            Local<v8::Object> sub = v8::Object::New();            

            sub->Set( v8::String::New( "time" ) , v8::Date::New( f.timestampTime() ) );
            sub->Set( v8::String::New( "i" ) , v8::Number::New( f.timestampInc() ) );
            
            o->Set( v8::String::New( f.fieldName() ) , sub );
            break;
        }
            
        case mongo::MinKey:
            // TODO: make a special type
            o->Set( v8::String::New( f.fieldName() ) , v8::String::New( "MinKey" ) );
            break;

        case mongo::MaxKey:
            // TODO: make a special type
            o->Set( v8::String::New( f.fieldName() ) , v8::String::New( "MaxKey" ) );
            break;
            
        default:
            cout << "can't handle type: ";
			cout  << f.type() << " ";
			cout  << f.toString();
			cout  << endl;
            break;
        }
        
    }

    return o;
}

void v8ToMongoElement( BSONObjBuilder & b , v8::Handle<v8::String> name , const string sname , v8::Handle<v8::Value> value ){
        
    if ( value->IsString() ){
        if ( sname == "$where" )
            b.appendCode( sname.c_str() , toSTLString( value ).c_str() );
        else
            b.append( sname.c_str() , toSTLString( value ).c_str() );
        return;
    }
        
    if ( value->IsFunction() ){
        b.appendCode( sname.c_str() , toSTLString( value ).c_str() );
        return;
    }
    
    if ( value->IsNumber() ){
        b.append( sname.c_str() , value->ToNumber()->Value() );
        return;
    }
    
    if ( value->IsArray() ){
        BSONObj sub = v8ToMongo( value->ToObject() );
        b.appendArray( sname.c_str() , sub );
        return;
    }
    
    if ( value->IsDate() ){
        b.appendDate( sname.c_str() , (unsigned long long )(v8::Date::Cast( *value )->NumberValue()) );
        return;
    }
    
    if ( value->IsObject() ){
        string s = toSTLString( value );
        if ( s.size() && s[0] == '/' ){
            s = s.substr( 1 );
            string r = s.substr( 0 , s.find( "/" ) );
            string o = s.substr( s.find( "/" ) + 1 );
            b.appendRegex( sname.c_str() , r.c_str() , o.c_str() );
        }
        else if ( value->ToObject()->GetPrototype()->IsObject() &&
                  value->ToObject()->GetPrototype()->ToObject()->HasRealNamedProperty( String::New( "isObjectId" ) ) ){
            OID oid;
            oid.init( toSTLString( value ) );
            b.appendOID( sname.c_str() , &oid );
        }
        else {
            BSONObj sub = v8ToMongo( value->ToObject() );
            b.append( sname.c_str() , sub );
        }
        return;
    }
    
    if ( value->IsBoolean() ){
        b.appendBool( sname.c_str() , value->ToBoolean()->Value() );
        return;
    }
    
    else if ( value->IsUndefined() ){
        return;
    }
    
    else if ( value->IsNull() ){
        b.appendNull( sname.c_str() );
        return;
    }

    cout << "don't know how to covert to mongo field [" << name << "]\t" << value << endl;
}

BSONObj v8ToMongo( v8::Handle<v8::Object> o ){
    BSONObjBuilder b;

    v8::Handle<v8::String> idName = String::New( "_id" );
    if ( o->HasRealNamedProperty( idName ) ){
        v8ToMongoElement( b , idName , "_id" , o->Get( idName ) );
    }
    
    Local<v8::Array> names = o->GetPropertyNames();
    for ( unsigned int i=0; i<names->Length(); i++ ){
        v8::Local<v8::String> name = names->Get(v8::Integer::New(i) )->ToString();

        if ( o->GetPrototype()->IsObject() &&
             o->GetPrototype()->ToObject()->HasRealNamedProperty( name ) )
            continue;
        
        v8::Local<v8::Value> value = o->Get( name );
        
        const string sname = toSTLString( name );
        if ( sname == "_id" )
            continue;

        v8ToMongoElement( b , name , sname , value );
    }
    return b.obj();
}

#ifdef _WIN32
#define GETNS char * ns = new char[args[0]->ToString()->Utf8Length()];  args[0]->ToString()->WriteUtf8( ns ); 
#else
#define GETNS char ns[args[0]->ToString()->Utf8Length()];  args[0]->ToString()->WriteUtf8( ns ); 
#endif

DBClientConnection * getConnection( const Arguments& args ){
    Local<External> c = External::Cast( *(args.This()->Get( CONN_STRING )) );
    DBClientConnection * conn = (DBClientConnection*)(c->Value());
    assert( conn );
    return conn;
}

// ---- real methods

/**
   0 - namespace
   1 - query
   2 - fields
   3 - limit
   4 - skip
 */
Handle<Value> mongoFind(const Arguments& args){
    jsassert( args.Length() == 5 , "find needs 5 args" );
    jsassert( args[1]->IsObject() , "needs to be an object" );
    DBClientConnection * conn = getConnection( args );
    GETNS;

    BSONObj q = v8ToMongo( args[1]->ToObject() );
    DDD( "query:" << q  );
    
    BSONObj fields;
    bool haveFields = args[2]->IsObject() && args[2]->ToObject()->GetPropertyNames()->Length() > 0;
    if ( haveFields )
        fields = v8ToMongo( args[2]->ToObject() );
    
    Local<v8::Object> mongo = args.This();
    Local<v8::Value> slaveOkVal = mongo->Get( String::New( "slaveOk" ) );
    jsassert( slaveOkVal->IsBoolean(), "slaveOk member invalid" );
    bool slaveOk = slaveOkVal->BooleanValue();
    
    try {
        auto_ptr<mongo::DBClientCursor> cursor;
        int nToReturn = (int)(args[3]->ToNumber()->Value());
        int nToSkip = (int)(args[4]->ToNumber()->Value());
        {
            v8::Unlocker u;
            cursor = conn->query( ns, q ,  nToReturn , nToSkip , haveFields ? &fields : 0, slaveOk ? Option_SlaveOk : 0 );
        }
        
        v8::Function * cons = (v8::Function*)( *( mongo->Get( String::New( "internalCursor" ) ) ) );
        Local<v8::Object> c = cons->NewInstance();
        
        // NOTE I don't believe the cursor object will ever be freed.
        c->Set( v8::String::New( "cursor" ) , External::New( cursor.release() ) );
        return c;
    }
    catch ( ... ){
        return v8::ThrowException( v8::String::New( "socket error on query" ) );        
    }
}

v8::Handle<v8::Value> mongoInsert(const v8::Arguments& args){
    jsassert( args.Length() == 2 , "insert needs 2 args" );
    jsassert( args[1]->IsObject() , "have to insert an object" );
    
    DBClientConnection * conn = getConnection( args );
    GETNS;
    
    v8::Handle<v8::Object> in = args[1]->ToObject();
    
    if ( ! in->Has( String::New( "_id" ) ) ){
        v8::Handle<v8::Value> argv[1];
        in->Set( String::New( "_id" ) , getObjectIdCons()->NewInstance( 0 , argv ) );
    }

    BSONObj o = v8ToMongo( in );

    DDD( "want to save : " << o.jsonString() );
    try {
        conn->insert( ns , o );
    }
    catch ( ... ){
        return v8::ThrowException( v8::String::New( "socket error on insert" ) );
    }
    
    return args[1];
}

v8::Handle<v8::Value> mongoRemove(const v8::Arguments& args){
    jsassert( args.Length() == 2 , "remove needs 2 args" );
    jsassert( args[1]->IsObject() , "have to remove an object template" );

    DBClientConnection * conn = getConnection( args );
    GETNS;
    
    v8::Handle<v8::Object> in = args[1]->ToObject();
    BSONObj o = v8ToMongo( in );
    
    DDD( "want to remove : " << o.jsonString() );
    try {
        conn->remove( ns , o );
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

    DBClientConnection * conn = getConnection( args );
    GETNS;
    
    v8::Handle<v8::Object> q = args[1]->ToObject();
    v8::Handle<v8::Object> o = args[2]->ToObject();
    
    bool upsert = args.Length() > 3 && args[3]->IsBoolean() && args[3]->ToBoolean()->Value();

    try {
        conn->update( ns , v8ToMongo( q ) , v8ToMongo( o ) , upsert );
    }
    catch ( ... ){
        return v8::ThrowException( v8::String::New( "socket error on remove" ) );
    }

    return v8::Undefined();
}




// --- cursor ---

mongo::DBClientCursor * getCursor( const Arguments& args ){
    Local<External> c = External::Cast( *(args.This()->Get( String::New( "cursor" ) ) ) );
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
    bool more;
    {
        v8::Unlocker u;
        more = cursor->more();
    }
    return Boolean::New( more );
}


// --- DB ----

v8::Handle<v8::Value> dbInit(const v8::Arguments& args){
    assert( args.Length() == 2 );

    args.This()->Set( String::New( "_mongo" ) , args[0] );
    args.This()->Set( String::New( "_name" ) , args[1] );

    for ( int i=0; i<args.Length(); i++ )
        assert( ! args[i]->IsUndefined() );

    return v8::Undefined();
}

v8::Handle<v8::Value> collectionInit( const v8::Arguments& args ){
    assert( args.Length() == 4 );

    args.This()->Set( String::New( "_mongo" ) , args[0] );
    args.This()->Set( String::New( "_db" ) , args[1] );
    args.This()->Set( String::New( "_shortName" ) , args[2] );
    args.This()->Set( String::New( "_fullName" ) , args[3] );
    
    for ( int i=0; i<args.Length(); i++ )
        assert( ! args[i]->IsUndefined() );

    return v8::Undefined();
}

v8::Handle<v8::Value> dbQueryInit( const v8::Arguments& args ){
    
    v8::Handle<v8::Object> t = args.This();

    assert( args.Length() >= 4 );
    
    t->Set( String::New( "_mongo" ) , args[0] );
    t->Set( String::New( "_db" ) , args[1] );
    t->Set( String::New( "_collection" ) , args[2] );
    t->Set( String::New( "_ns" ) , args[3] );

    if ( args.Length() > 4 && args[4]->IsObject() )
        t->Set( String::New( "_query" ) , args[4] );
    else 
        t->Set( String::New( "_query" ) , v8::Object::New() );
    
    if ( args.Length() > 5 && args[5]->IsObject() )
        t->Set( String::New( "_fields" ) , args[5] );
    else
        t->Set( String::New( "_fields" ) , v8::Null() );
    

    if ( args.Length() > 6 && args[6]->IsNumber() )
        t->Set( String::New( "_limit" ) , args[6] );
    else 
        t->Set( String::New( "_limit" ) , Number::New( 0 ) );

    if ( args.Length() > 7 && args[7]->IsNumber() )
        t->Set( String::New( "_skip" ) , args[7] );
    else 
        t->Set( String::New( "_skip" ) , Number::New( 0 ) );
    
    t->Set( String::New( "_cursor" ) , v8::Null() );
    t->Set( String::New( "_numReturned" ) , v8::Number::New(0) );
    t->Set( String::New( "_special" ) , Boolean::New(false) );
    
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

    v8::Handle<v8::Value> getCollection = info.This()->GetPrototype()->ToObject()->Get( String::New( "getCollection" ) );
    assert( getCollection->IsFunction() );

    v8::Function * f = (v8::Function*)(*getCollection);
    v8::Handle<v8::Value> argv[1];
    argv[0] = name;

    return f->Call( info.This() , 1 , argv );
}

v8::Handle<v8::Value> dbQueryIndexAccess( unsigned int index , const v8::AccessorInfo& info ){
    v8::Handle<v8::Value> arrayAccess = info.This()->GetPrototype()->ToObject()->Get( String::New( "arrayAccess" ) );
    assert( arrayAccess->IsFunction() );

    v8::Function * f = (v8::Function*)(*arrayAccess);
    v8::Handle<v8::Value> argv[1];
    argv[0] = v8::Number::New( index );

    return f->Call( info.This() , 1 , argv );    
}

v8::Function * getNamedCons( const char * name ){
    return v8::Function::Cast( *(v8::Context::GetCurrent()->Global()->Get( String::New( name ) ) ) );
}

v8::Function * getObjectIdCons(){
    return getNamedCons( "ObjectId" );
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
        oid.init( s );
    } 

    it->Set( String::New( "str" ) , String::New( oid.str().c_str() ) );
   
    return it;
}
