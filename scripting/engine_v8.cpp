//engine_v8.cpp 

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

#include "engine_v8.h"

#include "v8_wrapper.h"
#include "v8_utils.h"
#include "v8_db.h"

namespace mongo {

    // --- engine ---

    V8ScriptEngine::V8ScriptEngine()
    //        : _handleScope() , _globalTemplate( ObjectTemplate::New() ) 
{
        
        //_globalTemplate->Set(v8::String::New("print"), v8::FunctionTemplate::New(Print));
        //_globalTemplate->Set(v8::String::New("version"), v8::FunctionTemplate::New(Version));

        //_externalTemplate = getMongoFunctionTemplate( false );
        //_localTemplate = getMongoFunctionTemplate( true );
        //installDBTypes( _globalTemplate );
    }
    
    V8ScriptEngine::~V8ScriptEngine(){
    }

    void ScriptEngine::setup(){
        if ( !globalScriptEngine ){
            globalScriptEngine = new V8ScriptEngine();
        }
    }

    // --- scope ---
    
    V8Scope::V8Scope( V8ScriptEngine * engine ) 
        : _engine( engine ) , 
          _handleScope(),
          _context( Context::New() ) ,
          _scope( _context ) ,
          _global( _context->Global() ) ,
          _connectState( NOT ){
        _this = v8::Object::New();

        _global->Set(v8::String::New("print"), v8::FunctionTemplate::New(Print)->GetFunction() );
        _global->Set(v8::String::New("version"), v8::FunctionTemplate::New(Version)->GetFunction() );

        _global->Set(v8::String::New("load")
                    ,v8::FunctionTemplate::New(loadCallback, v8::External::New(this))->GetFunction() );

        //_externalTemplate = getMongoFunctionTemplate( false );
        //_localTemplate = getMongoFunctionTemplate( true );

        _wrapper = getObjectWrapperTemplate()->GetFunction();
        
        installDBTypes( _global );
    }

    V8Scope::~V8Scope(){
    }

    Handle< Value > V8Scope::nativeCallback( const Arguments &args ) {
        Local< External > f = External::Cast( *args.Callee()->Get( v8::String::New( "_native_function" ) ) );
        NativeFunction function = (NativeFunction)(f->Value());
        BSONObjBuilder b;
        for( int i = 0; i < args.Length(); ++i ) {
            stringstream ss;
            ss << i;
            v8ToMongoElement( b, v8::String::New( "foo" ), ss.str(), args[ i ] );
        }
        BSONObj nativeArgs = b.obj();
        BSONObj ret;
        try {
            ret = function( nativeArgs );
        } catch( const std::exception &e ) {
            return v8::ThrowException(v8::String::New(e.what()));
        } catch( ... ) {
            return v8::ThrowException(v8::String::New("unknown exception"));            
        }
        return mongoToV8Element( ret.firstElement() );
    }

    Handle< Value > V8Scope::loadCallback( const Arguments &args ) {
        HandleScope scope;
        Handle<External> field = Handle<External>::Cast(args.Data());
        void* ptr = field->Value();
        V8Scope* self = static_cast<V8Scope*>(ptr);

        Context::Scope context_scope(self->_context);
        for (int i = 0; i < args.Length(); ++i) {
            std::string filename(toSTLString(args[i]));
            if (!self->execFile(filename, false , true , false)) {
                return v8::ThrowException(v8::String::New((std::string("error loading file: ") + filename).c_str()));
            }
        }
        return v8::True();
    }

    // ---- global stuff ----

    void V8Scope::init( BSONObj * data ){
        if ( ! data )
            return;
        
        BSONObjIterator i( *data );
        while ( i.more() ){
            BSONElement e = i.next();
            setElement( e.fieldName() , e );
        }
    }
    
    void V8Scope::setNumber( const char * field , double val ){
        _global->Set( v8::String::New( field ) , v8::Number::New( val ) );
    }

    void V8Scope::setString( const char * field , const char * val ){
        _global->Set( v8::String::New( field ) , v8::String::New( val ) );
    }

    void V8Scope::setBoolean( const char * field , bool val ){
        _global->Set( v8::String::New( field ) , v8::Boolean::New( val ) );
    }

    void V8Scope::setElement( const char *field , const BSONElement& e ){ 
        _global->Set( v8::String::New( field ) , mongoToV8Element( e ) );
    }

    void V8Scope::setObject( const char *field , const BSONObj& obj , bool readOnly){
        // TODO: ignoring readOnly
        _global->Set( v8::String::New( field ) , mongoToV8( obj ) );
    }

    int V8Scope::type( const char *field ){
        Handle<Value> v = get( field );
        if ( v->IsNull() )
            return jstNULL;
        if ( v->IsUndefined() )
            return Undefined;
        if ( v->IsString() )
            return String;
        if ( v->IsFunction() )
            return Code;
        if ( v->IsArray() )
            return Array;
        if ( v->IsObject() )
            return Object;
        if ( v->IsBoolean() )
            return Bool;
        if ( v->IsInt32() )
            return NumberInt;
        if ( v->IsNumber() )
            return NumberDouble;
        if ( v->IsExternal() ){
            uassert( "can't handle external yet" , 0 );
            return -1;
        }
        if ( v->IsDate() )
            return Date;

        throw UserException( (string)"don't know what this is: " + field );
    }

    v8::Handle<v8::Value> V8Scope::get( const char * field ){
        return _global->Get( v8::String::New( field ) );
    }

    double V8Scope::getNumber( const char *field ){ 
        return get( field )->ToNumber()->Value();
    }

    int V8Scope::getNumberInt( const char *field ){ 
        return get( field )->ToInt32()->Value();
    }

    long long V8Scope::getNumberLongLong( const char *field ){ 
        return get( field )->ToInteger()->Value();
    }

    string V8Scope::getString( const char *field ){ 
        return toSTLString( get( field ) );
    }

    bool V8Scope::getBoolean( const char *field ){ 
        return get( field )->ToBoolean()->Value();
    }
    
    BSONObj V8Scope::getObject( const char * field ){
        Handle<Value> v = get( field );
        if ( v->IsNull() || v->IsUndefined() )
            return BSONObj();
        uassert( "not an object" , v->IsObject() );
        return v8ToMongo( v->ToObject() );
    }
    
    // --- functions -----

    ScriptingFunction V8Scope::_createFunction( const char * raw ){
        
        string code = raw;
        if ( code.find( "function" ) == string::npos ){
            if ( code.find( "\n" ) == string::npos && 
                 code.find( "return" ) == string::npos &&
                 ( code.find( ";" ) == string::npos || code.find( ";" ) == code.size() - 1 ) ){
                code = "return " + code;
            }
            code = "function(){ " + code + "}";
        }
        
        int num = _funcs.size() + 1;

        string fn;
        {
            stringstream ss;
            ss << "_funcs" << num;
            fn = ss.str();
        }
        
        code = fn + " = " + code;

        TryCatch try_catch;
        Handle<Script> script = v8::Script::Compile( v8::String::New( code.c_str() ) , 
                                                     v8::String::New( fn.c_str() ) );
        if ( script.IsEmpty() ){
            _error = (string)"compile error: " + toSTLString( &try_catch );
            log() << _error << endl;
            return 0;
        }
        
        Local<Value> result = script->Run();
        if ( result.IsEmpty() ){
            _error = (string)"compile error: " + toSTLString( &try_catch );
            log() << _error << endl;
            return 0;
        }        
        
        Handle<Value> f = _global->Get( v8::String::New( fn.c_str() ) );
        uassert( "not a func" , f->IsFunction() );
        _funcs.push_back( f );
        return num;
    }

    void V8Scope::setThis( const BSONObj * obj ){
        if ( ! obj ){
            _this = v8::Object::New();
            return;
        }

        //_this = mongoToV8( *obj );
        v8::Handle<v8::Value> argv[1];
        argv[0] = v8::External::New( createWrapperHolder( obj , true , false ) );
        _this = _wrapper->NewInstance( 1, argv );
    }
    
    int V8Scope::invoke( ScriptingFunction func , const BSONObj& argsObject, int timeoutMs , bool ignoreReturn ){
        Handle<Value> funcValue = _funcs[func-1];
        
        TryCatch try_catch;        
        int nargs = argsObject.nFields();
        auto_ptr< Handle<Value> > args;
        if ( nargs ){
            args.reset( new Handle<Value>[nargs] );
            BSONObjIterator it( argsObject );
            for ( int i=0; i<nargs; i++ ){
                BSONElement next = it.next();
                args.get()[i] = mongoToV8Element( next );
            }
        }
        Local<Value> result = ((v8::Function*)(*funcValue))->Call( _this , nargs , args.get() );
                
        if ( result.IsEmpty() ){
            stringstream ss;
            ss << "error in invoke: " << toSTLString( &try_catch );
            _error = ss.str();
            log() << _error << endl;
            return 1;
        }

        if ( ! ignoreReturn ){
            _global->Set( v8::String::New( "return" ) , result );
        }

        return 0;
    }

    bool V8Scope::exec( const string& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs ){

        if ( timeoutMs ){
            static bool t = 1;
            if ( t ){
                log() << "timeoutMs not support for v8 yet" << endl;
                t = 0;
            }
        }
        
        HandleScope handle_scope;
        TryCatch try_catch;
    
        Handle<Script> script = v8::Script::Compile( v8::String::New( code.c_str() ) , 
                                                     v8::String::New( name.c_str() ) );
        if (script.IsEmpty()) {
            stringstream ss;
            ss << "compile error: " << toSTLString( &try_catch );
            _error = ss.str();
            if (reportError)
                log() << _error << endl;
            if ( assertOnError )
                uassert( _error , 0 );
            return false;
        } 
    
        Handle<v8::Value> result = script->Run();
        if ( result.IsEmpty() ){
            _error = (string)"exec error: " + toSTLString( &try_catch );
            if ( reportError )
                log() << _error << endl;
            if ( assertOnError )
                uassert( _error , 0 );
            return false;
        } 
        
        _global->Set( v8::String::New( "__lastres__" ) , result );

        if ( printResult && ! result->IsUndefined() ){
            cout << toSTLString( result ) << endl;
        }
        
        return true;
    }

    // ----- db access -----

    void V8Scope::localConnect( const char * dbName ){
        if ( _connectState == EXTERNAL )
            throw UserException( "externalSetup already called, can't call externalSetup" );
        if ( _connectState ==  LOCAL ){
            if ( _localDBName == dbName )
                return;
            throw UserException( "localConnect called with a different name previously" );
        }

        //_global->Set( v8::String::New( "Mongo" ) , _engine->_externalTemplate->GetFunction() );
        _global->Set( v8::String::New( "Mongo" ) , getMongoFunctionTemplate( true )->GetFunction() );
        exec( jsconcatcode , "localConnect 1" , false , true , true , 0 );
        exec( "_mongo = new Mongo();" , "local connect 2" , false , true , true , 0 );
        exec( (string)"db = _mongo.getDB(\"" + dbName + "\");" , "local connect 3" , false , true , true , 0 );
        _connectState = LOCAL;
        _localDBName = dbName;
    }
    
    void V8Scope::externalSetup(){
        if ( _connectState == EXTERNAL )
            return;
        if ( _connectState == LOCAL )
            throw UserException( "localConnect already called, can't call externalSetup" );
        
        _global->Set( v8::String::New( "Mongo" ) , getMongoFunctionTemplate( false )->GetFunction() );
        exec( jsconcatcode , "shell setup" , false , true , true , 0 );
        _connectState = EXTERNAL;
    }

    // ----- internal -----

    void V8Scope::reset(){
        _startCall();
    }

    void V8Scope::_startCall(){
        _error = "";
        _context->Enter();
    }
    
} // namespace mongo
