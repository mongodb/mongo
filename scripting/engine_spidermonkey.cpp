// engine_spidermonkey.cpp

#include "engine_spidermonkey.h"

#include "../client/dbclient.h"

namespace mongo {

    boost::thread_specific_ptr<SMScope> currentScope( dontDeleteScope );
    
    class Convertor {
    public:
        Convertor( JSContext * cx ){
            _context = cx;
        }
        
        string toString( JSString * so ){
            jschar * s = JS_GetStringChars( so );
            size_t srclen = JS_GetStringLength( so );
            
            size_t len = srclen * 2;
            char * dst = (char*)malloc( len );
            assert( JS_EncodeCharacters( _context , s , srclen , dst , &len) );
            
            string ss( dst , len );
            free( dst );
            return ss;
        }

        string toString( jsval v ){
            return toString( JS_ValueToString( _context , v ) );            
        }

        double toNumber( jsval v ){
            double d;
            uassert( "not a number" , JS_ValueToNumber( _context , v , &d ) );
            return d;
        }

        bool toBoolean( jsval v ){
            JSBool b;
            assert( JS_ValueToBoolean( _context, v , &b ) );
            return b;
        }
        
        BSONObj toObject( JSObject * o ){
            if ( ! o )
                return BSONObj();
            
            BSONObjBuilder b;
            
            JSIdArray * properties = JS_Enumerate( _context , o );
            assert( properties );

            for ( jsint i=0; i<properties->length; i++ ){
                jsid id = properties->vector[i];
                jsval nameval;
                assert( JS_IdToValue( _context ,id , &nameval ) );
                string name = toString( nameval );
                append( b , name , getProperty( o , name.c_str() ) );
            }
            
            return b.obj();
        }
        
        BSONObj toObject( jsval v ){
            if ( JSVAL_IS_NULL( v ) || 
                 JSVAL_IS_VOID( v ) )
                return BSONObj();
            
            uassert( "not an object" , JSVAL_IS_OBJECT( v ) );
            return toObject( JSVAL_TO_OBJECT( v ) );
        }
        
        void append( BSONObjBuilder& b , string name , jsval val ){
            switch ( JS_TypeOfValue( _context , val ) ){

            case JSTYPE_VOID: b.appendUndefined( name.c_str() ); break;
            case JSTYPE_NULL: b.appendNull( name.c_str() ); break;
                
            case JSTYPE_NUMBER: b.append( name.c_str() , toNumber( val ) ); break;
            case JSTYPE_STRING: b.append( name.c_str() , toString( val ) ); break;

            default: uassert( (string)"can't append type: " + typeString( val ) , 0 );
            }
        }

        // ---------- to spider monkey ---------
        
        jsval toval( double d ){
            jsval val;
            assert( JS_NewNumberValue( _context, d , &val ) );
            return val;
        }

        jsval toval( const char * c ){
            JSString * s = JS_NewStringCopyZ( _context , c );
            return STRING_TO_JSVAL( s );
        }

        JSObject * toJSObject( const BSONObj * obj , bool readOnly=true ){
            // TODO: check this

            JSObject * o = JS_NewObject( _context , &bson_ro_class , NULL, NULL);

            JS_SetPrivate( _context , o , (void*)(new BSONObj( obj->getOwned() ) ) );
            return o;
        }
        
        jsval toval( const BSONObj* obj , bool readOnly=true ){
            JSObject * o = toJSObject( obj , readOnly );
            return OBJECT_TO_JSVAL( o );
        }

        jsval toval( const BSONElement& e ){

            switch( e.type() ){
            case EOO:
            case jstNULL:
            case Undefined:
                return JSVAL_NULL;
            case NumberDouble:
            case NumberInt:
                return toval( e.number() );
            case String:
                return toval( e.valuestr() );
            case Bool:
                return e.boolean() ? JSVAL_TRUE : JSVAL_FALSE;
            case Object:{
                BSONObj embed = e.embeddedObject();
                return toval( &embed , true );
            }
            case Array:{

                BSONObj embed = e.embeddedObject();
                
                int n = embed.nFields();
                JSObject * array = JS_NewArrayObject( _context , embed.nFields() , 0 );
                assert( array );

                for ( int i=0; i<n; i++ ){
                    jsval v = toval( embed[BSONObjBuilder::numStr( i ).c_str()] );
                    assert( JS_SetElement( _context , array , i , &v ) );
                }
                
                return OBJECT_TO_JSVAL( array );
            }
            case jstOID:{
                OID oid = e.__oid();
                JSObject * o = JS_NewObject( _context , &object_id_class , 0 , 0 );
                setProperty( o , "str" , toval( oid.str().c_str() ) );
                return OBJECT_TO_JSVAL( o );
            }
            case RegEx:{
                const char * flags = e.regexFlags();
                uintN flagNumber = 0;
                while ( *flags ){
                    switch ( *flags ){
                    case 'g': flagNumber |= JSREG_GLOB; break;
                    case 'i': flagNumber |= JSREG_FOLD; break;
                    case 'm': flagNumber |= JSREG_MULTILINE; break;
                        //case 'y': flagNumber |= JSREG_STICKY; break;
                    default: uassert( "unknown regex flag" , 0 );
                    }
                    flags++;
                }

                JSObject * r = JS_NewRegExpObject( _context , (char*)e.regex() , strlen( e.regex() ) , flagNumber );
                assert( r );
                return OBJECT_TO_JSVAL( r );
            }
            case Date: 
                return OBJECT_TO_JSVAL( js_NewDateObjectMsec( _context , e.date() ) );

            default:
                log() << "toval can't handle type: " << (int)(e.type()) << endl;
            }
            
            uassert( "not done: toval" , 0 );
            return 0;
        }
        
        // ------- object helpers ------

        JSObject * getJSObject( JSObject * o , const char * name ){
            jsval v;
            assert( JS_GetProperty( _context , o , name , &v ) );
            return JSVAL_TO_OBJECT( v );
        }
        
        JSObject * getGlobalObject( const char * name ){
            return getJSObject( JS_GetGlobalObject( _context ) , name );
        }

        JSObject * getGlobalPrototype( const char * name ){
            return getJSObject( getGlobalObject( name ) , "prototype" );
        }
        
        bool hasProperty( JSObject * o , const char * name ){
            JSBool res;
            assert( JS_HasProperty( _context , o , name , & res ) );
            return res;
        }

        jsval getProperty( JSObject * o , const char * field ){
            uassert( "object passed to getPropery is null" , o );
            jsval v;
            assert( JS_GetProperty( _context , o , field , &v ) );
            return v;
        }

        void setProperty( JSObject * o , const char * field , jsval v ){
            assert( JS_SetProperty( _context , o , field , &v ) );
        }
        
        string typeString( jsval v ){
            JSType t = JS_TypeOfValue( _context , v );
            return JS_GetTypeName( _context , t );
        }
        
        bool getBoolean( JSObject * o , const char * field ){
            return toBoolean( getProperty( o , field ) );
        }

    private:
        JSContext * _context;
    };


    void bson_finalize( JSContext * cx , JSObject * obj ){
        BSONObj * o = (BSONObj*)JS_GetPrivate( cx , obj );
        if ( o ){
            delete o;
            JS_SetPrivate( cx , obj , 0 );
        }
    }

    JSBool bson_enumerate( JSContext *cx, JSObject *obj, JSIterateOp enum_op, jsval *statep, jsid *idp ){

        if ( enum_op == JSENUMERATE_INIT ){
            BSONObjIterator * it = new BSONObjIterator( ((BSONObj*)JS_GetPrivate( cx , obj ))->getOwned() );
            *statep = PRIVATE_TO_JSVAL( it );
            if ( idp )
                *idp = JSVAL_ZERO;
            return JS_TRUE;
        }
        
        BSONObjIterator * it = (BSONObjIterator*)JSVAL_TO_PRIVATE( *statep );
        
        if ( enum_op == JSENUMERATE_NEXT ){
            if ( it->more() ){
                BSONElement e = it->next();
                if ( e.eoo() ){
                    *statep = 0;
                }
                else {
                    Convertor c(cx);
                    assert( JS_ValueToId( cx , c.toval( e.fieldName() ) , idp ) );
                }
            }
            else {
                *statep = 0;
            }
            return JS_TRUE;
        }
        
        if ( enum_op == JSENUMERATE_DESTROY ){
            delete it;
            return JS_TRUE;
        }
        
        uassert( "don't know what to do with this op" , 0 );
        return JS_FALSE;
    }

    
    JSClass bson_ro_class = {
        "bson_object" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE , 
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        (JSEnumerateOp)bson_enumerate, (JSResolveOp)(&resolveBSONField) , JS_ConvertStub, bson_finalize ,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };



    static JSClass global_class = {
        "global", JSCLASS_GLOBAL_FLAGS,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };    

    // --- global helpers ---

    JSBool native_print( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval ){
        Convertor c( cx );
        for ( uintN i=0; i<argc; i++ ){
            if ( i > 0 )
                cout << " ";
            cout << c.toString( argv[i] );
        }
        cout << endl;
        return JS_TRUE;
    }

    JSFunctionSpec globalHelpers[] = { 
        { "print" , &native_print , 0 , 0 , 0 } , 
        { 0 , 0 , 0 , 0 , 0 } 
    };

    // ----END global helpers ----

    
    JSBool resolveBSONField( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ){
        Convertor c( cx );
        
        BSONObj * o = (BSONObj*)(JS_GetPrivate( cx , obj ));
        string s = c.toString( id );
       
        BSONElement e = (*o)[ s.c_str() ];

        if ( e.type() == EOO ){
            *objp = 0;
            return JS_TRUE;
        }
        
        jsval val = c.toval( e );

        assert( JS_SetProperty( cx , obj , s.c_str() , &val ) );
        *objp = obj;
        return JS_TRUE;
    }
    

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

        Scope * createScope();
        
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


    // ------ special helpers -------
    
    JSBool object_keyset(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        
        JSIdArray * properties = JS_Enumerate( cx , obj );
        assert( properties );

        JSObject * array = JS_NewArrayObject( cx , properties->length , 0 );
        assert( array );
        
        for ( jsint i=0; i<properties->length; i++ ){
            jsid id = properties->vector[i];
            jsval idval;
            assert( JS_IdToValue( cx , id , &idval ) );
            assert( JS_SetElement( cx , array , i ,  &idval ) );
        }
        
        *rval = OBJECT_TO_JSVAL( array );
        return JS_TRUE;
    }
    
    // ------ scope ------

        
    class SMScope : public Scope {
    public:
        SMScope(){
            _context = JS_NewContext( globalSMEngine->_runtime , 8192 );
            _convertor = new Convertor( _context );
            massert( "JS_NewContext failed" , _context );
            
            JS_SetOptions( _context , JSOPTION_VAROBJFIX);
            //JS_SetVersion( _context , JSVERSION_LATEST); TODO
            JS_SetErrorReporter( _context , errorReporter );
            
            _global = JS_NewObject( _context , &global_class, NULL, NULL);
            massert( "JS_NewObject failed for global" , _global );
            massert( "js init failed" , JS_InitStandardClasses( _context , _global ) );
            
            JS_DefineFunctions( _context , _global , globalHelpers );
            
            // install my special helpers
            
            assert( JS_DefineFunction( _context , _convertor->getGlobalPrototype( "Object" ) , 
                                       "keySet" , object_keyset , 0 , JSPROP_READONLY ) );

            _this = 0;
        }

        ~SMScope(){
            uassert( "deleted SMScope twice?" , _convertor );
            
            if ( _convertor ){
                delete _convertor;
                _convertor = 0;
            }

            if ( _context ){
                JS_DestroyContext( _context );
                _context = 0;
            }
        }
        
        void reset(){
            massert( "not implemented yet" , 0 );
        }
        
        void init( BSONObj * data ){
            massert( "not implemented yet" , 0 );            
        }

        void localConnect( const char * dbName ){
            initMongoJS( this , _context , _global , true );
            
            exec( "_mongo = new Mongo();" );
            exec( ((string)"db = _mongo.getDB( \"" + dbName + "\" ); ").c_str() );
        }
        
        // ----- getters ------
        double getNumber( const char *field ){
            jsval val;
            assert( JS_GetProperty( _context , _global , field , &val ) );
            return _convertor->toNumber( val );
        }
        
        string getString( const char *field ){
            jsval val;
            assert( JS_GetProperty( _context , _global , field , &val ) );
            JSString * s = JS_ValueToString( _context , val );
            return _convertor->toString( s );
        }

        bool getBoolean( const char *field ){
            return _convertor->getBoolean( _global , field );
        }
        
        BSONObj getObject( const char *field ){
            return _convertor->toObject( _convertor->getProperty( _global , field ) );
        }

        JSObject * getJSObject( const char * field ){
            return _convertor->getJSObject( _global , field );
        }

        int type( const char *field ){
            jsval val;
            assert( JS_GetProperty( _context , _global , field , &val ) );
            
            switch ( JS_TypeOfValue( _context , val ) ){
            case JSTYPE_VOID: return Undefined;
            case JSTYPE_NULL: return jstNULL;
            case JSTYPE_OBJECT:	return Object;
            case JSTYPE_FUNCTION: return Code;
            case JSTYPE_STRING: return String;
            case JSTYPE_NUMBER: return NumberDouble;
            case JSTYPE_BOOLEAN: return Bool;
            default:
                uassert( "unknown type" , 0 );
            }
            return 0;
        }

        // ----- setters ------
        
        void setNumber( const char *field , double val ){
            jsval v = _convertor->toval( val );
            assert( JS_SetProperty( _context , _global , field , &v ) );
        }

        void setString( const char *field , const char * val ){
            jsval v = _convertor->toval( val );
            assert( JS_SetProperty( _context , _global , field , &v ) );
        }

        void setObject( const char *field , const BSONObj& obj ){
            jsval v = _convertor->toval( &obj );
            JS_SetProperty( _context , _global , field , &v );
        }

        void setBoolean( const char *field , bool val ){
            jsval v = BOOLEAN_TO_JSVAL( val );
            assert( JS_SetProperty( _context , _global , field , &v ) );            
        }

        void setThis( const BSONObj * obj ){
            _this = _convertor->toJSObject( obj );
        }

        // ---- functions -----
        
        bool hasFunctionIdentifier( const string& code ){
            if ( code.size() < 9 || code.find( "function" ) != 0  )
                return false;
            
            return code[8] == ' ' || code[8] == '(';
        }

        JSFunction * compileFunction( const char * code ){
            if ( ! hasFunctionIdentifier( code ) ){
                string s = code;
                if ( strstr( code , "return" ) == 0 )
                    s = "return " + s;
                return JS_CompileFunction( _context , 0 , "anonymous" , 0 , 0 , s.c_str() , strlen( s.c_str() ) , "nofile" , 0 );
            }
            
            // TODO: there must be a way in spider monkey to do this - this is a total hack

            string s = "return ";
            s += code;
            s += ";";

            JSFunction * func = JS_CompileFunction( _context , 0 , "anonymous" , 0 , 0 , s.c_str() , strlen( s.c_str() ) , "nofile" , 0 );
            jsval ret;
            JS_CallFunction( _context , 0 , func , 0 , 0 , &ret );
            return JS_ValueToFunction( _context , ret );
        }

        ScriptingFunction createFunction( const char * code ){
            return (ScriptingFunction)compileFunction( code );
        }

        void precall(){
            _error = "";
            currentScope.reset( this );
        }
        
        void exec( const char * code ){
            precall();
            jsval ret;
            assert( JS_EvaluateScript( _context , _global , code , strlen( code ) , "anon" , 0 , &ret ) );
        }

        int invoke( JSFunction * func , const BSONObj& args ){
            precall();
            jsval rval;
            
            int nargs = args.nFields();
            jsval smargs[nargs];
            
            BSONObjIterator it( args );
            for ( int i=0; i<nargs; i++ )
                smargs[i] = _convertor->toval( it.next() );
            
            setObject( "args" , args ); // this is for backwards compatability

            if ( ! JS_CallFunction( _context , _this , func , nargs , smargs , &rval ) ){
                return -3;
            }

            assert( JS_SetProperty( _context , _global , "return" , &rval ) );
            return 0;
        }

        int invoke( ScriptingFunction funcAddr , const BSONObj& args ){
            return invoke( (JSFunction*)funcAddr , args );
        }

        void gotError( string s ){
            _error = s;
        }
        
        string getError(){
            return _error;
        }

    private:
        JSContext * _context;
        Convertor * _convertor;

        JSObject * _global;
        JSObject * _this;

        string _error;
    };

    void errorReporter( JSContext *cx, const char *message, JSErrorReport *report ){
        stringstream ss;
        ss << "JS Error: " << message;
        
        if ( report ){
            ss << " " << report->filename << ":" << report->lineno;
        }
        
        log() << ss.str() << endl;
        currentScope->gotError( ss.str() );
        
        // TODO: send to Scope
    }



    void SMEngine::runTest(){
        SMScope s;
        
        s.localConnect( "foo" );

        s.exec( "assert( db.getMongo() )" );
        s.exec( "assert( db.bar , 'collection getting does not work' ); " );
        s.exec( "assert.eq( db._name , 'foo' );" );
        s.exec( "assert( _mongo == db.getMongo() ); " );
        s.exec( "assert( _mongo == db._mongo ); " );
        s.exec( "assert( typeof DB.bar == 'undefined' ); " );
        s.exec( "assert( typeof DB.prototype.bar == 'undefined' , 'resolution is happening on prototype, not object' ); " );

        s.exec( "assert( db.bar ); " );
        s.exec( "assert( typeof db.addUser == 'function' )" );
        s.exec( "assert( db.addUser == DB.prototype.addUser )" );
        s.exec( "assert.eq( 'foo.bar' , db.bar._fullName ); " );
        s.exec( "db.bar.verify();" );

        s.exec( "db.bar.silly.verify();" );
        s.exec( "assert.eq( 'foo.bar.silly' , db.bar.silly._fullName )" );
        s.exec( "assert.eq( 'function' , typeof _mongo.find , 'mongo.find is not a function' )" );
    }

    Scope * SMEngine::createScope(){
        return new SMScope();
    }
    
    
}

#include "sm_db.cpp"
