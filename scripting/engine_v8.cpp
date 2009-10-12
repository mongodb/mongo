#include "engine_v8.h"

#include "v8_wrapper.h"
#include "v8_utils.h"

namespace mongo {

    // --- engine ---

    V8ScriptEngine::V8ScriptEngine()
        : _handleScope() , _globalTemplate( ObjectTemplate::New() ) {
        
        _globalTemplate->Set(v8::String::New("print"), v8::FunctionTemplate::New(Print));
        _globalTemplate->Set(v8::String::New("version"), v8::FunctionTemplate::New(Version));
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
        : _handleScope(),
          _context( Context::New( 0 , engine->_globalTemplate ) ) ,
          _scope( _context ) ,
          _global( _context->Global() ){
        _this = v8::Object::New();
    }

    V8Scope::~V8Scope(){
        
    }

    Handle< Value > V8Scope::nativeCallback( const Arguments &args ) {
        Local< External > f = External::Cast( *args.Callee()->Get( v8::String::New( "_native_function" ) ) );
        NativeFunction function = ( NativeFunction )( f->Value() );
        BSONObjBuilder b;
        for( int i = 0; i < args.Length(); ++i ) {
            stringstream ss;
            ss << i;
            v8ToMongoElement( b, v8::String::New( "foo" ), ss.str(), args[ i ] );
        }
        BSONObj ret;
        try {
            ret = function( b.done() );
        } catch( const std::exception &e ) {
            return v8::ThrowException(v8::String::New(e.what()));
        } catch( ... ) {
            return v8::ThrowException(v8::String::New("unknown exception"));            
        }
        return mongoToV8Element( ret.firstElement() );
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

    void V8Scope::setThis( const BSONObj * obj ){
        _this = mongoToV8( *obj );
    }

    double V8Scope::getNumber( const char *field ){ 
        return _global->Get( v8::String::New( field ) )->ToNumber()->Value();
    }

    string V8Scope::getString( const char *field ){ 
        return toSTLString( _global->Get( v8::String::New( field ) ) );
    }

    bool V8Scope::getBoolean( const char *field ){ 
        return _global->Get( v8::String::New( field ) )->ToBoolean()->Value();
    }

    ScriptingFunction V8Scope::_createFunction( const char * raw ){
        
        string code = raw;
        if ( code.find( "function" ) == string::npos ){
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
            stringstream ss;
            ss << "exec error: " << &try_catch;
            _error = ss.str();
            if ( reportError )
                log() << _error << endl;
            if ( assertOnError )
                uassert( _error , 0 );
            return false;
        } 
        
        if ( printResult ){
            cout << toSTLString( result ) << endl;
        }
        
        return true;
    }

    void V8Scope::_startCall(){
        _error = "";
    }
    
} // namespace mongo
