#include "engine_v8.h"

#include "../shell/MongoJS.h"

namespace mongo {

    void ScriptEngine::setup(){
        if ( !globalScriptEngine ){
            globalScriptEngine = new V8ScriptEngine();
        }
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

} // namespace mongo
