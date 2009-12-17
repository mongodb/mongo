// v8_wrapper.cpp

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

#include <iostream>

using namespace std;
using namespace v8;

namespace mongo {

#define CONN_STRING (v8::String::New( "_conn" ))

#define DDD(x)

    Handle<Value> NamedReadOnlySet( Local<v8::String> property, Local<Value> value, const AccessorInfo& info ) {
        cout << "cannot write to read-only object" << endl;
        return value;
    }

    Handle<Boolean> NamedReadOnlyDelete( Local<v8::String> property, const AccessorInfo& info ) {
        cout << "cannot delete from read-only object" << endl;
        return Boolean::New( false );
    }
    
    Handle<Value> IndexedReadOnlySet( uint32_t index, Local<Value> value, const AccessorInfo& info ) {
        cout << "cannot write to read-only array" << endl;
        return value;
    }
    
    Handle<Boolean> IndexedReadOnlyDelete( uint32_t index, const AccessorInfo& info ) {
        cout << "cannot delete from read-only array" << endl;
        return Boolean::New( false );
    }
    
    Local< v8::Value > newFunction( const char *code ) {
        stringstream codeSS;
        codeSS << "____MontoToV8_newFunction_temp = " << code;
        string codeStr = codeSS.str();
        Local< Script > compiled = Script::New( v8::String::New( codeStr.c_str() ) );
        Local< Value > ret = compiled->Run();
        return ret;
    }
    
    Local< v8::Value > newId( const OID &id ) {
        v8::Function * idCons = getObjectIdCons();
        v8::Handle<v8::Value> argv[1];
        argv[0] = v8::String::New( id.str().c_str() );
        return idCons->NewInstance( 1 , argv );        
    }
    
    Local<v8::Object> mongoToV8( const BSONObj& m , bool array, bool readOnly ){

        // handle DBRef. needs to come first. isn't it? (metagoto)
        static string ref = "$ref";
        if ( ref == m.firstElement().fieldName() ) {
            const BSONElement& id = m["$id"];
            if (!id.eoo()) { // there's no check on $id exitence in sm implementation. risky ?
                v8::Function* dbRef = getNamedCons( "DBRef" );
                v8::Handle<v8::Value> argv[2];
                argv[0] = mongoToV8Element(m.firstElement());
                argv[1] = mongoToV8Element(m["$id"]);
                return dbRef->NewInstance(2, argv);
            }
        }

        Local< v8::ObjectTemplate > readOnlyObjects;
        // Hoping template construction is fast...
        Local< v8::ObjectTemplate > internalFieldObjects = v8::ObjectTemplate::New();
        internalFieldObjects->SetInternalFieldCount( 1 );

        Local<v8::Object> o;
        if ( array ) {
            // NOTE Looks like it's impossible to add interceptors to non array objects in v8.
            o = v8::Array::New();
        } else if ( !readOnly ) {
            o = v8::Object::New();
        } else {
            // NOTE Our readOnly implemention relies on undocumented ObjectTemplate
            // functionality that may be fragile, but it still seems like the best option
            // for now -- fwiw, the v8 docs are pretty sparse.  I've determined experimentally
            // that when property handlers are set for an object template, they will attach
            // to objects previously created by that template.  To get this to work, though,
            // it is necessary to initialize the template's property handlers before
            // creating objects from the template (as I have in the following few lines
            // of code).
            // NOTE In my first attempt, I configured the permanent property handlers before
            // constructiong the object and replaced the Set() calls below with ForceSet().
            // However, it turns out that ForceSet() only bypasses handlers for named
            // properties and not for indexed properties.
            readOnlyObjects = v8::ObjectTemplate::New();
            // NOTE This internal field will store type info for special db types.  For
            // regular objects the field is unnecessary - for simplicity I'm creating just
            // one readOnlyObjects template for objects where the field is & isn't necessary,
            // assuming that the overhead of an internal field is slight.
            readOnlyObjects->SetInternalFieldCount( 1 );
            readOnlyObjects->SetNamedPropertyHandler( 0 );
            readOnlyObjects->SetIndexedPropertyHandler( 0 );
            o = readOnlyObjects->NewInstance();
        }
        
        mongo::BSONObj sub;

        for ( BSONObjIterator i(m); i.more(); ) {
            const BSONElement& f = i.next();
        
            Local<Value> v;
        
            switch ( f.type() ){

            case mongo::Code:
                o->Set( v8::String::New( f.fieldName() ), newFunction( f.valuestr() ) );
                break;

            case CodeWScope:
                if ( f.codeWScopeObject().isEmpty() )
                    log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
                o->Set( v8::String::New( f.fieldName() ), newFunction( f.codeWScopeCode() ) );
                break;
            
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
                o->Set( v8::String::New( f.fieldName() ) , mongoToV8( sub , f.type() == mongo::Array, readOnly ) );
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
                Local<v8::Object> b = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();

                int len;
                const char *data = f.binData( len );
            
                b->Set( v8::String::New( "subtype" ) , v8::Number::New( f.binDataType() ) );
                b->Set( v8::String::New( "length" ) , v8::Number::New( len ) );
                b->Set( v8::String::New( "data" ) , v8::String::New( data, len ) );
                b->SetInternalField( 0, v8::Uint32::New( f.type() ) );
                
                o->Set( v8::String::New( f.fieldName() ) , b );
                break;
            }
            
            case mongo::Timestamp: {
                Local<v8::Object> sub = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();
                
                sub->Set( v8::String::New( "time" ) , v8::Date::New( f.timestampTime() ) );
                sub->Set( v8::String::New( "i" ) , v8::Number::New( f.timestampInc() ) );
                sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
                
                o->Set( v8::String::New( f.fieldName() ) , sub );
                break;
            }
            
            case mongo::MinKey: {
                Local<v8::Object> sub = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();
                sub->Set( v8::String::New( "$MinKey" ), v8::Boolean::New( true ) );
                sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
                o->Set( v8::String::New( f.fieldName() ) , sub );
                break;
            }
                    
            case mongo::MaxKey: {
                Local<v8::Object> sub = readOnly ? readOnlyObjects->NewInstance() : internalFieldObjects->NewInstance();
                sub->Set( v8::String::New( "$MaxKey" ), v8::Boolean::New( true ) );
                sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
                o->Set( v8::String::New( f.fieldName() ) , sub );
                break;
            }

            case mongo::DBRef: {
                v8::Function* dbPointer = getNamedCons( "DBPointer" );
                v8::Handle<v8::Value> argv[2];
                argv[0] = v8::String::New( f.dbrefNS() );
                argv[1] = newId( f.dbrefOID() );
                o->Set( v8::String::New( f.fieldName() ), dbPointer->NewInstance(2, argv) );
                break;
            }
                    
            default:
                cout << "can't handle type: ";
                cout  << f.type() << " ";
                cout  << f.toString();
                cout  << endl;
                break;
            }
        
        }

        if ( !array && readOnly ) {
            readOnlyObjects->SetNamedPropertyHandler( 0, NamedReadOnlySet, 0, NamedReadOnlyDelete );
            readOnlyObjects->SetIndexedPropertyHandler( 0, IndexedReadOnlySet, 0, IndexedReadOnlyDelete );            
        }
        
        return o;
    }

    Handle<v8::Value> mongoToV8Element( const BSONElement &f ) {
        Local< v8::ObjectTemplate > internalFieldObjects = v8::ObjectTemplate::New();
        internalFieldObjects->SetInternalFieldCount( 1 );

        switch ( f.type() ){

        case mongo::Code:
            return newFunction( f.valuestr() );
                
        case CodeWScope:
            if ( f.codeWScopeObject().isEmpty() )
                log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
            return newFunction( f.codeWScopeCode() );
                
        case mongo::String: 
            return v8::String::New( f.valuestr() );
            
        case mongo::jstOID:
            return newId( f.__oid() );
            
        case mongo::NumberDouble:
        case mongo::NumberInt:
            return v8::Number::New( f.number() );
            
        case mongo::Array:
        case mongo::Object:
            return mongoToV8( f.embeddedObject() , f.type() == mongo::Array );
            
        case mongo::Date:
            return v8::Date::New( f.date() );
            
        case mongo::Bool:
            return v8::Boolean::New( f.boolean() );

        case mongo::EOO:            
        case mongo::jstNULL:
            return v8::Null();
            
        case mongo::RegEx: {
            v8::Function * regex = getNamedCons( "RegExp" );
            
            v8::Handle<v8::Value> argv[2];
            argv[0] = v8::String::New( f.regex() );
            argv[1] = v8::String::New( f.regexFlags() );
            
            return regex->NewInstance( 2 , argv );
            break;
        }
            
        case mongo::BinData: {
            Local<v8::Object> b = internalFieldObjects->NewInstance();
            
            int len;
            const char *data = f.binData( len );
            
            b->Set( v8::String::New( "subtype" ) , v8::Number::New( f.binDataType() ) );
            b->Set( v8::String::New( "length" ) , v8::Number::New( len ) );
            b->Set( v8::String::New( "data" ) , v8::String::New( data, len ) );
            b->SetInternalField( 0, v8::Uint32::New( f.type() ) );
            
            return b;
        };
            
        case mongo::Timestamp: {
            Local<v8::Object> sub = internalFieldObjects->NewInstance();
            
            sub->Set( v8::String::New( "time" ) , v8::Date::New( f.timestampTime() ) );
            sub->Set( v8::String::New( "i" ) , v8::Number::New( f.timestampInc() ) );
            sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );

            return sub;
        }
            
        case mongo::MinKey: {
            Local<v8::Object> sub = internalFieldObjects->NewInstance();
            sub->Set( v8::String::New( "$MinKey" ), v8::Boolean::New( true ) );
            sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
            return sub;
        }
            
        case mongo::MaxKey: {
            Local<v8::Object> sub = internalFieldObjects->NewInstance();
            sub->Set( v8::String::New( "$MaxKey" ), v8::Boolean::New( true ) );
            sub->SetInternalField( 0, v8::Uint32::New( f.type() ) );
            return sub;
        }
                
        case mongo::Undefined:
            return v8::Undefined();

        case mongo::DBRef: {
            v8::Function* dbPointer = getNamedCons( "DBPointer" );
            v8::Handle<v8::Value> argv[2];
            argv[0] = v8::String::New( f.dbrefNS() );
            argv[1] = newId( f.dbrefOID() );
            return dbPointer->NewInstance(2, argv);
        }
                       
        default:
            cout << "can't handle type: ";
			cout  << f.type() << " ";
			cout  << f.toString();
			cout  << endl;
            break;
        }    
        
        return v8::Undefined();
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
            if ( value->IsInt32() )
                b.append( sname.c_str(), int( value->ToInt32()->Value() ) );
            else
                b.append( sname.c_str() , value->ToNumber()->Value() );
            return;
        }
    
        if ( value->IsArray() ){
            BSONObj sub = v8ToMongo( value->ToObject() );
            b.appendArray( sname.c_str() , sub );
            return;
        }
    
        if ( value->IsDate() ){
            b.appendDate( sname.c_str() , Date_t(v8::Date::Cast( *value )->NumberValue()) );
            return;
        }

        if ( value->IsExternal() )
            return;
        
        if ( value->IsObject() ){
            // The user could potentially modify the fields of these special objects,
            // wreaking havoc when we attempt to reinterpret them.  Not doing any validation
            // for now...
            Local< v8::Object > obj = value->ToObject();
            if ( obj->InternalFieldCount() && obj->GetInternalField( 0 )->IsNumber() ) {
                switch( obj->GetInternalField( 0 )->ToInt32()->Value() ) { // NOTE Uint32's Value() gave me a linking error, so going with this instead
                    case Timestamp:
                        b.appendTimestamp( sname.c_str(),
                                          Date_t( v8::Date::Cast( *obj->Get( v8::String::New( "time" ) ) )->NumberValue() ),
                                          obj->Get( v8::String::New( "i" ) )->ToInt32()->Value() );
                        return;
                    case MinKey:
                        b.appendMinKey( sname.c_str() );
                        return;
                    case MaxKey:
                        b.appendMaxKey( sname.c_str() );
                        return;
                    case BinData: {
                        int len = obj->Get( v8::String::New( "length" ) )->ToInt32()->Value();
                        v8::String::Utf8Value data( obj->Get( v8::String::New( "data" ) ) );
                        const char *dataArray = *data;
                        assert( data.length() == len );
                        b.appendBinData( sname.c_str(),
                                        len,
                                        mongo::BinDataType( obj->Get( v8::String::New( "subtype" ) )->ToInt32()->Value() ),
                                        dataArray );
                        return;
                    }
                    default:
                        assert( "invalid internal field" == 0 );
                }
            }
            string s = toSTLString( value );
            if ( s.size() && s[0] == '/' ){
                s = s.substr( 1 );
                string r = s.substr( 0 , s.find( "/" ) );
                string o = s.substr( s.find( "/" ) + 1 );
                b.appendRegex( sname.c_str() , r.c_str() , o.c_str() );
            }
            else if ( value->ToObject()->GetPrototype()->IsObject() &&
                      value->ToObject()->GetPrototype()->ToObject()->HasRealNamedProperty( v8::String::New( "isObjectId" ) ) ){
                OID oid;
                oid.init( toSTLString( value ) );
                b.appendOID( sname.c_str() , &oid );
            }
            else if ( !value->ToObject()->GetHiddenValue( v8::String::New( "__DBPointer" ) ).IsEmpty() ) {
                // TODO might be nice to potentially speed this up with an indexed internal
                // field, but I don't yet know how to use an ObjectTemplate with a
                // constructor.
                OID oid;
                oid.init( toSTLString( value->ToObject()->Get( v8::String::New( "id" ) ) ) );
                string ns = toSTLString( value->ToObject()->Get( v8::String::New( "ns" ) ) );
                b.appendDBRef( sname.c_str(), ns.c_str(), oid );                
            } else {
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

        cout << "don't know how to convert to mongo field [" << name << "]\t" << value << endl;
    }

    BSONObj v8ToMongo( v8::Handle<v8::Object> o ){
        BSONObjBuilder b;

        v8::Handle<v8::String> idName = v8::String::New( "_id" );
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

    // --- object wrapper ---

    class WrapperHolder {
    public:
        WrapperHolder( const BSONObj * o , bool readOnly , bool iDelete )
            : _o(o), _readOnly( readOnly ), _iDelete( iDelete ) {
        }
        
        ~WrapperHolder(){
            if ( _o && _iDelete ){
                delete _o;
            }
            _o = 0;
        }

        v8::Handle<v8::Value> get( v8::Local<v8::String> name ){
            const string& s = toSTLString( name );
            const BSONElement& e = _o->getField( s );
            return mongoToV8Element(e);
        }

        const BSONObj * _o;
        bool _readOnly;
        bool _iDelete;
    };

    WrapperHolder * createWrapperHolder( const BSONObj * o , bool readOnly , bool iDelete ){
        return new WrapperHolder( o , readOnly , iDelete );
    }

#define WRAPPER_STRING (v8::String::New( "_wrapper" ) )

    WrapperHolder * getWrapper( v8::Handle<v8::Object> o ){
        Handle<v8::Value> t = o->GetRealNamedProperty( WRAPPER_STRING );
        assert( t->IsExternal() );
        Local<External> c = External::Cast( *t );
        WrapperHolder * w = (WrapperHolder*)(c->Value());
        assert( w );
        return w;
    }


    Handle<Value> wrapperCons(const Arguments& args){
        if ( ! ( args.Length() == 1 && args[0]->IsExternal() ) )
            return v8::ThrowException( v8::String::New( "wrapperCons needs 1 External arg" ) );

        args.This()->Set( WRAPPER_STRING , args[0] );
        
        return v8::Undefined();
    }

    v8::Handle<v8::Value> wrapperGetHandler( v8::Local<v8::String> name, const v8::AccessorInfo &info){
        return getWrapper( info.This() )->get( name );
    }

    v8::Handle<v8::FunctionTemplate> getObjectWrapperTemplate(){
        v8::Local<v8::FunctionTemplate> t = FunctionTemplate::New( wrapperCons );
        t->InstanceTemplate()->SetNamedPropertyHandler( wrapperGetHandler );
        return t;
    }

    // --- random utils ----

    v8::Function * getNamedCons( const char * name ){
        return v8::Function::Cast( *(v8::Context::GetCurrent()->Global()->Get( v8::String::New( name ) ) ) );
    }

    v8::Function * getObjectIdCons(){
        return getNamedCons( "ObjectId" );
    }

}
