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

#define V8_SIMPLE_HEADER Locker l; HandleScope handle_scope; Context::Scope context_scope( _context );

namespace mongo {

    // --- engine ---

    V8ScriptEngine::V8ScriptEngine() {}
    
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
          _connectState( NOT ){

        Locker l;
        HandleScope handleScope;              
        _context = Context::New();
        Context::Scope context_scope( _context );
        _global = Persistent< v8::Object >::New( _context->Global() );

        _this = Persistent< v8::Object >::New( v8::Object::New() );

        _global->Set(v8::String::New("print"), newV8Function< Print >()->GetFunction() );
        _global->Set(v8::String::New("version"), newV8Function< Version >()->GetFunction() );

        _global->Set(v8::String::New("load"),
                     v8::FunctionTemplate::New( v8Callback< loadCallback >, v8::External::New(this))->GetFunction() );
        
        _wrapper = Persistent< v8::Function >::New( getObjectWrapperTemplate()->GetFunction() );
        
        _global->Set(v8::String::New("gc"), newV8Function< GCV8 >()->GetFunction() );


        installDBTypes( _global );
    }

    V8Scope::~V8Scope(){
        Locker l;
        Context::Scope context_scope( _context );        
        _wrapper.Dispose();
        _this.Dispose();
        for( unsigned i = 0; i < _funcs.size(); ++i )
            _funcs[ i ].Dispose();
        _funcs.clear();
        _global.Dispose();
        _context.Dispose();
    }

    Handle< Value > V8Scope::nativeCallback( const Arguments &args ) {
        Locker l;
        HandleScope handle_scope;
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
        return handle_scope.Close( mongoToV8Element( ret.firstElement() ) );
    }

    Handle< Value > V8Scope::loadCallback( const Arguments &args ) {
        Locker l;
        HandleScope handle_scope;
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
        Locker l;
        if ( ! data )
            return;
        
        BSONObjIterator i( *data );
        while ( i.more() ){
            BSONElement e = i.next();
            setElement( e.fieldName() , e );
        }
    }
    
    void V8Scope::setNumber( const char * field , double val ){
        V8_SIMPLE_HEADER
        _global->Set( v8::String::New( field ) , v8::Number::New( val ) );
    }

    void V8Scope::setString( const char * field , const char * val ){
        V8_SIMPLE_HEADER
        _global->Set( v8::String::New( field ) , v8::String::New( val ) );
    }

    void V8Scope::setBoolean( const char * field , bool val ){
        V8_SIMPLE_HEADER
        _global->Set( v8::String::New( field ) , v8::Boolean::New( val ) );
    }

    void V8Scope::setElement( const char *field , const BSONElement& e ){ 
        V8_SIMPLE_HEADER
        _global->Set( v8::String::New( field ) , mongoToV8Element( e ) );
    }

    void V8Scope::setObject( const char *field , const BSONObj& obj , bool readOnly){
        V8_SIMPLE_HEADER
        // Set() accepts a ReadOnly parameter, but this just prevents the field itself
        // from being overwritten and doesn't protect the object stored in 'field'.
        _global->Set( v8::String::New( field ) , mongoToV8( obj, false, readOnly) );
    }

    int V8Scope::type( const char *field ){
        V8_SIMPLE_HEADER
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
        if ( v->IsBoolean() )
            return Bool;
        if ( v->IsInt32() )
            return NumberInt;
        if ( v->IsNumber() )
            return NumberDouble;
        if ( v->IsExternal() ){
            uassert( 10230 ,  "can't handle external yet" , 0 );
            return -1;
        }
        if ( v->IsDate() )
            return Date;
        if ( v->IsObject() )
            return Object;

        throw UserException( 12509, (string)"don't know what this is: " + field );
    }

    v8::Handle<v8::Value> V8Scope::get( const char * field ){
        return _global->Get( v8::String::New( field ) );
    }

    double V8Scope::getNumber( const char *field ){ 
        V8_SIMPLE_HEADER
        return get( field )->ToNumber()->Value();
    }

    int V8Scope::getNumberInt( const char *field ){
        V8_SIMPLE_HEADER
        return get( field )->ToInt32()->Value();
    }

    long long V8Scope::getNumberLongLong( const char *field ){ 
        V8_SIMPLE_HEADER
        return get( field )->ToInteger()->Value();
    }

    string V8Scope::getString( const char *field ){ 
        V8_SIMPLE_HEADER
        return toSTLString( get( field ) );
    }

    bool V8Scope::getBoolean( const char *field ){ 
        V8_SIMPLE_HEADER
        return get( field )->ToBoolean()->Value();
    }
    
    BSONObj V8Scope::getObject( const char * field ){
        V8_SIMPLE_HEADER
        Handle<Value> v = get( field );
        if ( v->IsNull() || v->IsUndefined() )
            return BSONObj();
        uassert( 10231 ,  "not an object" , v->IsObject() );
        return v8ToMongo( v->ToObject() );
    }
    
    // --- functions -----

    Local< v8::Function > V8Scope::__createFunction( const char * raw ){
        for(; isspace( *raw ); ++raw ); // skip whitespace
        string code = raw;
        if ( code.find( "function" ) == string::npos ){
            if ( code.find( "\n" ) == string::npos && 
                 ! hasJSReturn( code ) && 
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
            return Local< v8::Function >();
        }
        
        Local<Value> result = script->Run();
        if ( result.IsEmpty() ){
            _error = (string)"compile error: " + toSTLString( &try_catch );
            log() << _error << endl;
            return Local< v8::Function >();
        }        
     
        return v8::Function::Cast( *_global->Get( v8::String::New( fn.c_str() ) ) );
    }
    
    ScriptingFunction V8Scope::_createFunction( const char * raw ){
        V8_SIMPLE_HEADER
        Local< Value > ret = __createFunction( raw );
        if ( ret.IsEmpty() )
            return 0;
        Persistent<Value> f = Persistent< Value >::New( ret );
        uassert( 10232, "not a func" , f->IsFunction() );
        int num = _funcs.size() + 1;
        _funcs.push_back( f );
        return num;
    }

    void V8Scope::setThis( const BSONObj * obj ){
        V8_SIMPLE_HEADER
        if ( ! obj ){
            _this = Persistent< v8::Object >::New( v8::Object::New() );
            return;
        }

        //_this = mongoToV8( *obj );
        v8::Handle<v8::Value> argv[1];
        argv[0] = v8::External::New( createWrapperHolder( obj , true , false ) );
        _this = Persistent< v8::Object >::New( _wrapper->NewInstance( 1, argv ) );
    }

    void V8Scope::rename( const char * from , const char * to ){
        V8_SIMPLE_HEADER;
        v8::Local<v8::String> f = v8::String::New( from );
        v8::Local<v8::String> t = v8::String::New( to );
        _global->Set( t , _global->Get( f ) );
        _global->Set( f , v8::Undefined() );
    }
    
    int V8Scope::invoke( ScriptingFunction func , const BSONObj& argsObject, int timeoutMs , bool ignoreReturn ){
        V8_SIMPLE_HEADER
        Handle<Value> funcValue = _funcs[func-1];
        
        TryCatch try_catch;        
        int nargs = argsObject.nFields();
        scoped_array< Handle<Value> > args;
        if ( nargs ){
            args.reset( new Handle<Value>[nargs] );
            BSONObjIterator it( argsObject );
            for ( int i=0; i<nargs; i++ ){
                BSONElement next = it.next();
                args[i] = mongoToV8Element( next );
            }
            setObject( "args", argsObject, true ); // for backwards compatibility
        } else {
            _global->Set( v8::String::New( "args" ), v8::Undefined() );
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

    bool V8Scope::exec( const StringData& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs ){
        if ( timeoutMs ){
            static bool t = 1;
            if ( t ){
                log() << "timeoutMs not support for v8 yet  code: " << code << endl;
                t = 0;
            }
        }
        
        V8_SIMPLE_HEADER
        
        TryCatch try_catch;
    
        Handle<Script> script = v8::Script::Compile( v8::String::New( code.data() ) , 
                                                     v8::String::New( name.c_str() ) );
        if (script.IsEmpty()) {
            stringstream ss;
            ss << "compile error: " << toSTLString( &try_catch );
            _error = ss.str();
            if (reportError)
                log() << _error << endl;
            if ( assertOnError )
                uassert( 10233 ,  _error , 0 );
            return false;
        } 
    
        Handle<v8::Value> result = script->Run();
        if ( result.IsEmpty() ){
            _error = (string)"exec error: " + toSTLString( &try_catch );
            if ( reportError )
                log() << _error << endl;
            if ( assertOnError )
                uassert( 10234 ,  _error , 0 );
            return false;
        } 
        
        _global->Set( v8::String::New( "__lastres__" ) , result );

        if ( printResult && ! result->IsUndefined() ){
            cout << toSTLString( result ) << endl;
        }
        
        return true;
    }
    
    void V8Scope::injectNative( const char *field, NativeFunction func ){
        V8_SIMPLE_HEADER
        
        Handle< FunctionTemplate > f( newV8Function< nativeCallback >() );
        f->Set( v8::String::New( "_native_function" ), External::New( (void*)func ) );
        _global->Set( v8::String::New( field ), f->GetFunction() );
    }        
    
    void V8Scope::gc() {
        cout << "in gc" << endl;
        Locker l;
        while( V8::IdleNotification() );
    }

    // ----- db access -----

    void V8Scope::localConnect( const char * dbName ){
        {
            V8_SIMPLE_HEADER

            if ( _connectState == EXTERNAL )
                throw UserException( 12510, "externalSetup already called, can't call externalSetup" );
            if ( _connectState ==  LOCAL ){
                if ( _localDBName == dbName )
                    return;
                throw UserException( 12511, "localConnect called with a different name previously" );
            }

            //_global->Set( v8::String::New( "Mongo" ) , _engine->_externalTemplate->GetFunction() );
            _global->Set( v8::String::New( "Mongo" ) , getMongoFunctionTemplate( true )->GetFunction() );
            execCoreFiles();
            exec( "_mongo = new Mongo();" , "local connect 2" , false , true , true , 0 );
            exec( (string)"db = _mongo.getDB(\"" + dbName + "\");" , "local connect 3" , false , true , true , 0 );
            _connectState = LOCAL;
            _localDBName = dbName;
        }
        loadStored();
    }
    
    void V8Scope::externalSetup(){
        V8_SIMPLE_HEADER
        if ( _connectState == EXTERNAL )
            return;
        if ( _connectState == LOCAL )
            throw UserException( 12512, "localConnect already called, can't call externalSetup" );

        installFork( _global, _context );
        _global->Set( v8::String::New( "Mongo" ) , getMongoFunctionTemplate( false )->GetFunction() );
        execCoreFiles();
        _connectState = EXTERNAL;
    }

    // ----- internal -----

    void V8Scope::reset(){
        _startCall();
    }

    void V8Scope::_startCall(){
        _error = "";
    }
    
} // namespace mongo
