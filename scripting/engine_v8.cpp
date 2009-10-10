#include "engine_v8.h"

#include "v8_wrapper.h"
#include "v8_utils.h"

namespace mongo {

    V8ScriptEngine::V8ScriptEngine()
        : _handleScope() , _globalTemplate( ObjectTemplate::New() ) {
        
    }

    V8ScriptEngine::~V8ScriptEngine(){
    }

    void ScriptEngine::setup(){
        if ( !globalScriptEngine ){
            globalScriptEngine = new V8ScriptEngine();
        }
    }

    V8Scope::V8Scope( V8ScriptEngine * engine ) 
        : _handleScope(),
          _context( Context::New( 0 , engine->_globalTemplate ) ) ,
          _scope( _context ) ,
          _global( _context->Global() ){
        
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

    double V8Scope::getNumber( const char *field ){ 
        return _global->Get( v8::String::New( field ) )->ToNumber()->Value();
    }

    string V8Scope::getString( const char *field ){ 
        return toSTLString( _global->Get( v8::String::New( field ) ) );
    }

    bool V8Scope::getBoolean( const char *field ){ 
        return _global->Get( v8::String::New( field ) )->ToBoolean()->Value();
    }
    
} // namespace mongo
