// engine_spidermonkey.cpp

#include "engine.h"

#include "js/jsapi.h"

namespace mongo {

    static JSClass global_class = {
        "global", JSCLASS_GLOBAL_FLAGS,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };    

    class SMScope;
    
    class SMEngine : public ScriptEngine {
    public:
        
        SMEngine(){
            _runtime = JS_NewRuntime(8L * 1024L * 1024L);
            uassert( "JS_NewRuntime failed" , _runtime );

        }

        ~SMEngine(){
            JS_DestroyRuntime( _runtime );
            JS_ShutDown();
        }

        Scope * createScope(){
            uassert( "not done" , 0 );
            return 0;
        }
        
        void runTest();
       
    private:
        JSRuntime * _runtime;
        friend class SMScope;
    };
    
    SMEngine * globalSMEngine;


    void ScriptEngine::setup(){
        globalSMEngine = new SMEngine();
        globalScriptEngine = globalSMEngine;
    }


    class SMScope {
    public:
        SMScope(){
            _context = JS_NewContext( globalSMEngine->_runtime , 8192 );
            massert( "JS_NewContext failed" , _context );
            
            JS_SetOptions( _context , JSOPTION_VAROBJFIX);
            //JS_SetVersion( _context , JSVERSION_LATEST); TODO
            //JS_SetErrorReporter( _context , reportError); TODO
            
            _global = JS_NewObject( _context , &global_class, NULL, NULL);
            massert( "JS_NewObject failed for global" , _global );
            massert( "js init failed" , JS_InitStandardClasses( _context , _global ) == 0 );
        }

        ~SMScope(){
            JS_DestroyContext( _context );
        }

        // ----- getters ------
        double getNumber( const char *field ){
            jsval val;
            assert( JS_GetProperty( _context , _global , field , &val ) );
            return toNumber( val );
        }
        
        double toNumber( jsval v ){
            double d;
            uassert( "not a number" , JS_ValueToNumber( _context , v , &d ) );
            return d;
        }

        // ---- functions -----
        
        JSFunction * compileFunction( const char * code ){
            return JS_CompileFunction( _context , 0 , "anonymous" , 0 , 0 , code , strlen( code ) , "nofile" , 0 );
        }

        ScriptingFunction createFunction( const char * code ){
            return (ScriptingFunction)compileFunction( code );
        }
        
        int invoke( JSFunction * func , const BSONObj& args ){
            jsval rval;
            JS_CallFunction( _context , 0 , func , 0 , 0 , &rval );
            return 0;
        }

        int invoke( ScriptingFunction funcAddr , const BSONObj& args ){
            return invoke( (JSFunction*)funcAddr , args );
        }

    private:
        JSContext * _context;
        JSObject * _global;
    };

    void SMEngine::runTest(){
        {
            SMScope scope;
            ScriptingFunction func = scope.createFunction( "x = 5" );
            scope.invoke( func , BSONObj() );
            assert( scope.getNumber( "x" ) == 5 );
        }
    }

}
