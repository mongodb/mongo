// engine_spidermonkey.cpp

#include "stdafx.h"
#include "engine_spidermonkey.h"

#include "../client/dbclient.h"

namespace mongo {

    boost::thread_specific_ptr<SMScope> currentScope( dontDeleteScope );

#define GETHOLDER(x,o) ((BSONHolder*)JS_GetPrivate( x , o ))

    class BSONFieldIterator;
    
    class BSONHolder {
    public:
        
        BSONHolder( BSONObj obj ){
            _obj = obj.getOwned();
            _inResolve = false;
            _modified = false;
            _magic = 17;
        }
        
        void check(){
            uassert( "holder magic value is wrong" , _magic == 17 );
        }

        BSONFieldIterator * it();

        BSONObj _obj;
        bool _inResolve;
        char _magic;
        list<string> _extra;
        bool _modified;
    };

    class BSONFieldIterator {
    public:
        
        BSONFieldIterator( BSONHolder * holder ){

            BSONObjIterator it( holder->_obj );
            while ( it.more() ){
                BSONElement e = it.next();
                if ( e.eoo() )
                    break;
                _names.push_back( e.fieldName() );
            }
            
            _names.merge( holder->_extra );
            
            _it = _names.begin();
        }
        
        bool more(){
            return _it != _names.end();
        }

        string next(){
            string s = *_it;
            _it++;
            return s;
        }
        
    private:
        list<string> _names;
        list<string>::iterator _it;
    };

    BSONFieldIterator * BSONHolder::it(){
        return new BSONFieldIterator( this );
    }


    class Convertor : boost::noncopyable {
    public:
        Convertor( JSContext * cx ){
            _context = cx;
        }
        
        string toString( JSString * so ){
            jschar * s = JS_GetStringChars( so );
            size_t srclen = JS_GetStringLength( so );
            if( srclen == 0 )
                return "";
            
            size_t len = srclen * 6; // we only need *3, but see note on len below
            char * dst = (char*)malloc( len );
            
            len /= 2;
            // doc re weird JS_EncodeCharacters api claims len expected in 16bit
            // units, but experiments suggest 8bit units expected.  We allocate
            // enough memory that either will work.

            assert( JS_EncodeCharacters( _context , s , srclen , dst , &len) );

            string ss( dst , len );
            free( dst );
            if ( !JS_CStringsAreUTF8() )
                for( string::const_iterator i = ss.begin(); i != ss.end(); ++i )
                    uassert( "non ascii character detected", (unsigned char)(*i) <= 127 );
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
            
            if ( JS_InstanceOf( _context , o , &bson_ro_class , 0 ) ){
                return GETHOLDER( _context , o )->_obj.getOwned();
            }

            BSONObj orig;
            if ( JS_InstanceOf( _context , o , &bson_class , 0 ) ){
                BSONHolder * holder = GETHOLDER(_context,o);
                if ( ! holder->_modified )
                    return holder->_obj;
                orig = holder->_obj;
            }
            
            BSONObjBuilder b;
            
            jsval theid = getProperty( o , "_id" );
            if ( ! JSVAL_IS_VOID( theid ) ){
                append( b , "_id" , theid );
            }
            
            JSIdArray * properties = JS_Enumerate( _context , o );
            assert( properties );

            for ( jsint i=0; i<properties->length; i++ ){
                jsid id = properties->vector[i];
                jsval nameval;
                assert( JS_IdToValue( _context ,id , &nameval ) );
                string name = toString( nameval );
                if ( name == "_id" )
                    continue;
                
                append( b , name , getProperty( o , name.c_str() ) , orig[name].type() );
            }
                        
            JS_DestroyIdArray( _context , properties );

            return b.obj();
        }
        
        BSONObj toObject( jsval v ){
            if ( JSVAL_IS_NULL( v ) || 
                 JSVAL_IS_VOID( v ) )
                return BSONObj();
            
            uassert( "not an object" , JSVAL_IS_OBJECT( v ) );
            return toObject( JSVAL_TO_OBJECT( v ) );
        }
        
        string getFunctionCode( JSFunction * func ){
            return toString( JS_DecompileFunction( _context , func , 0 ) );
        }

        string getFunctionCode( jsval v ){
            uassert( "not a function" , JS_TypeOfValue( _context , v ) == JSTYPE_FUNCTION );
            return getFunctionCode( JS_ValueToFunction( _context , v ) );
        }

        void append( BSONObjBuilder& b , string name , jsval val , BSONType oldType = EOO  ){
            //cout << "name: " << name << "\t" << typeString( val ) << " oldType: " << oldType << endl;
            switch ( JS_TypeOfValue( _context , val ) ){
                
            case JSTYPE_VOID: b.appendUndefined( name.c_str() ); break;
            case JSTYPE_NULL: b.appendNull( name.c_str() ); break;
                
            case JSTYPE_NUMBER: {
                double d = toNumber( val );
                if ( oldType == NumberInt && ((int)d) == d )
                    b.append( name.c_str() , (int)d );
                else
                    b.append( name.c_str() , d );
                break;
            }
            case JSTYPE_STRING: b.append( name.c_str() , toString( val ) ); break;
            case JSTYPE_BOOLEAN: b.appendBool( name.c_str() , toBoolean( val ) ); break;

            case JSTYPE_OBJECT: {
                JSObject * o = JSVAL_TO_OBJECT( val );
                if ( ! o || o == JSVAL_NULL ){
                    b.appendNull( name.c_str() );
                }
                else if ( ! appendSpecialDBObject( this , b , name , o ) ){
                    BSONObj sub = toObject( o );
                    if ( JS_IsArrayObject( _context , o ) ){
                        b.appendArray( name.c_str() , sub );
                    }
                    else {
                        b.append( name.c_str() , sub );
                    }
                }
                break;
            }

            case JSTYPE_FUNCTION: {
                string s = toString(val);
                if ( s[0] == '/' ){
                    s = s.substr(1);
                    string::size_type end = s.rfind( '/' );
                    b.appendRegex( name.c_str() , s.substr( 0 , end ).c_str() , s.substr( end + 1 ).c_str() );
                }
                else {
                    b.appendCode( name.c_str() , getFunctionCode( val ).c_str() ); 
                }
                break;
            }
                
            default: uassert( (string)"can't append field.  name:" + name + " type: " + typeString( val ) , 0 );
            }
        }
        
        // ---------- to spider monkey ---------

        bool hasFunctionIdentifier( const string& code ){
            if ( code.size() < 9 || code.find( "function" ) != 0  )
                return false;
            
            return code[8] == ' ' || code[8] == '(';
        }

        bool isSimpleStatement( const string& code ){
            if ( code.find( "return" ) != string::npos )
                return false;

            if ( code.find( ";" ) != string::npos &&
                 code.find( ";" ) != code.rfind( ";" ) )
                return false;

            if ( code.find( "for(" ) != string::npos ||
                 code.find( "for (" ) != string::npos ||
                 code.find( "while (" ) != string::npos ||
                 code.find( "while(" ) != string::npos )
                return false;

            return true;
        }

        void addRoot( JSFunction * f );

        JSFunction * compileFunction( const char * code, JSObject * assoc = 0 ){
            JSFunction * f = _compileFunction( code , assoc );
            addRoot( f );
            return f;
        }
        
        JSFunction * _compileFunction( const char * code, JSObject * assoc ){
            if ( ! hasFunctionIdentifier( code ) ){
                string s = code;
                if ( isSimpleStatement( s ) ){
                    s = "return " + s;
                }
                return JS_CompileFunction( _context , assoc , "anonymous" , 0 , 0 , s.c_str() , strlen( s.c_str() ) , "nofile_a" , 0 );
            }
            
            // TODO: there must be a way in spider monkey to do this - this is a total hack

            string s = "return ";
            s += code;
            s += ";";

            JSFunction * func = JS_CompileFunction( _context , assoc , "anonymous" , 0 , 0 , s.c_str() , strlen( s.c_str() ) , "nofile_b" , 0 );
            if ( ! func ){
                cerr << "compile for hack failed" << endl;
                return 0;
            }
            
            jsval ret;
            if ( ! JS_CallFunction( _context , 0 , func , 0 , 0 , &ret ) ){
                cerr << "call function for hack failed" << endl;
                return 0;
            }

            addRoot( func );

            uassert( "return for compile hack failed" , JS_TypeOfValue( _context , ret ) == JSTYPE_FUNCTION );
            return JS_ValueToFunction( _context , ret );
        }

        
        jsval toval( double d ){
            jsval val;
            assert( JS_NewNumberValue( _context, d , &val ) );
            return val;
        }

        jsval toval( const char * c ){
            JSString * s = JS_NewStringCopyZ( _context , c );
            assert( s );
            return STRING_TO_JSVAL( s );
        }

        JSObject * toJSObject( const BSONObj * obj , bool readOnly=false ){
            JSObject * o = JS_NewObject( _context , readOnly ? &bson_ro_class : &bson_class , NULL, NULL);
            assert( o );
            assert( JS_SetPrivate( _context , o , (void*)(new BSONHolder( obj->getOwned() ) ) ) );
            return o;
        }
        
        jsval toval( const BSONObj* obj , bool readOnly=false ){
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
                BSONObj embed = e.embeddedObject().getOwned();
                return toval( &embed );
            }
            case Array:{
            
                BSONObj embed = e.embeddedObject().getOwned();
                
                if ( embed.isEmpty() ){
                    return OBJECT_TO_JSVAL( JS_NewArrayObject( _context , 0 , 0 ) );
                }

                int n = embed.nFields();
                assert( n > 0 );

                JSObject * array = JS_NewArrayObject( _context , embed.nFields() , 0 );
                assert( array );
                
                jsval myarray = OBJECT_TO_JSVAL( array );
                
                for ( int i=0; i<n; i++ ){
                    jsval v = toval( embed[i] );
                    assert( JS_SetElement( _context , array , i , &v ) );
                }
                
                return myarray;
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
            case Code:{
                JSFunction * func = compileFunction( e.valuestr() );
                return OBJECT_TO_JSVAL( JS_GetFunctionObject( func ) );
            }
            case CodeWScope:{
                JSFunction * func = compileFunction( e.codeWScopeCode() );
                
                BSONObj extraScope = e.codeWScopeObject();
                if ( ! extraScope.isEmpty() ){
                    log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
                }

                return OBJECT_TO_JSVAL( JS_GetFunctionObject( func ) );
            }
            case Date: 
                return OBJECT_TO_JSVAL( js_NewDateObjectMsec( _context , (jsdouble) e.date() ) );
                
            case MinKey:
                return OBJECT_TO_JSVAL( JS_NewObject( _context , &minkey_class , 0 , 0 ) );

            case MaxKey:
                return OBJECT_TO_JSVAL( JS_NewObject( _context , &maxkey_class , 0 , 0 ) );

            case Timestamp: {
                JSObject * o = JS_NewObject( _context , &timestamp_class , 0 , 0 );
                setProperty( o , "t" , toval( (double)(e.timestampTime()) ) );
                setProperty( o , "i" , toval( (double)(e.timestampInc()) ) );
                return OBJECT_TO_JSVAL( o );
            }

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

        double getNumber( JSObject * o , const char * field ){
            return toNumber( getProperty( o , field ) );
        }
        
        string getString( JSObject * o , const char * field ){
            return toString( getProperty( o , field ) );
        }

        JSContext * _context;
    };


    void bson_finalize( JSContext * cx , JSObject * obj ){
        BSONHolder * o = GETHOLDER( cx , obj );
        if ( o ){
            delete o;
            assert( JS_SetPrivate( cx , obj , 0 ) );
        }
    }

    JSBool bson_enumerate( JSContext *cx, JSObject *obj, JSIterateOp enum_op, jsval *statep, jsid *idp ){

        if ( enum_op == JSENUMERATE_INIT ){
            BSONFieldIterator * it = GETHOLDER( cx , obj )->it();
            *statep = PRIVATE_TO_JSVAL( it );
            if ( idp )
                *idp = JSVAL_ZERO;
            return JS_TRUE;
        }
        
        BSONFieldIterator * it = (BSONFieldIterator*)JSVAL_TO_PRIVATE( *statep );
        
        if ( enum_op == JSENUMERATE_NEXT ){
            if ( it->more() ){
                string name = it->next();
                Convertor c(cx);
                assert( JS_ValueToId( cx , c.toval( name.c_str() ) , idp ) );
            }
            else {
                delete it;
                *statep = 0;
            }
            return JS_TRUE;
        }
        
        if ( enum_op == JSENUMERATE_DESTROY ){
            if ( it ) 
                delete it;
            return JS_TRUE;
        }
        
        uassert( "don't know what to do with this op" , 0 );
        return JS_FALSE;
    }
    
    JSBool noaccess( JSContext *cx, JSObject *obj, jsval idval, jsval *vp){
        BSONHolder * holder = GETHOLDER( cx , obj );
        if ( holder->_inResolve )
            return JS_TRUE;
        JS_ReportError( cx , "doing write op on read only operation" );
        return JS_FALSE;
    }
    
    JSClass bson_ro_class = {
        "bson_ro_object" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE , 
        noaccess, noaccess, JS_PropertyStub, noaccess,
        (JSEnumerateOp)bson_enumerate, (JSResolveOp)(&resolveBSONField) , JS_ConvertStub, bson_finalize ,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    JSBool bson_add_prop( JSContext *cx, JSObject *obj, jsval idval, jsval *vp){
        BSONHolder * holder = GETHOLDER( cx , obj );
        if ( ! holder->_inResolve ){
            Convertor c(cx);
            holder->_extra.push_back( c.toString( idval ) );
            holder->_modified = true;
        }
        return JS_TRUE;
    }

    
    JSBool mark_modified( JSContext *cx, JSObject *obj, jsval idval, jsval *vp){
        BSONHolder * holder = GETHOLDER( cx , obj );
        if ( holder->_inResolve )
            return JS_TRUE;
        holder->_modified = true;
        return JS_TRUE;
    }

    JSClass bson_class = {
        "bson_object" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE , 
        bson_add_prop, mark_modified, JS_PropertyStub, mark_modified,
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

    JSBool native_helper( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ){
        Convertor c(cx);
        uassert( "native_helper needs at least 1 arg" , argc >= 1 );

        NativeFunction func = (NativeFunction)JSVAL_TO_PRIVATE( argv[0] );

        BSONObjBuilder args;
        for ( uintN i=1; i<argc; i++ ){
            c.append( args , args.numStr( i ) , argv[i] );
        }

        BSONObj out = func( args.obj() );
        
        if ( out.isEmpty() ){
            *rval = JSVAL_VOID;
        }
        else {
            *rval = c.toval( out.firstElement() );
        }

        return JS_TRUE;
    }

    JSBool native_load( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval );

    JSBool native_gc( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ){
        JS_GC( cx );
        return JS_TRUE;
    }

    JSFunctionSpec globalHelpers[] = { 
        { "print" , &native_print , 0 , 0 , 0 } , 
        { "nativeHelper" , &native_helper , 1 , 0 , 0 } , 
        { "load" , &native_load , 1 , 0 , 0 } , 
        { "gc" , &native_gc , 1 , 0 , 0 } , 
        
        { 0 , 0 , 0 , 0 , 0 } 
    };

    // ----END global helpers ----

    
    JSBool resolveBSONField( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ){
        assert( JS_EnterLocalRootScope( cx ) );
        Convertor c( cx );
        
        BSONHolder * holder = GETHOLDER( cx , obj );
        holder->check();
        
        string s = c.toString( id );
        
        BSONElement e = holder->_obj[ s.c_str() ];

        if ( e.type() == EOO ){
            *objp = 0;
            JS_LeaveLocalRootScope( cx );
            return JS_TRUE;
        }
        
        jsval val = c.toval( e );

        assert( ! holder->_inResolve );
        holder->_inResolve = true;
        assert( JS_SetProperty( cx , obj , s.c_str() , &val ) );
        holder->_inResolve = false;

        *objp = obj;
        JS_LeaveLocalRootScope( cx );
        return JS_TRUE;
    }
    

    class SMScope;
    
    class SMEngine : public ScriptEngine {
    public:
        
        SMEngine(){
            _runtime = JS_NewRuntime(8L * 1024L * 1024L);
            uassert( "JS_NewRuntime failed" , _runtime );
            if ( ! utf8Ok() ){
                cerr << "*** warning: spider monkey build without utf8 support" << endl;
            }
        }

        ~SMEngine(){
            JS_DestroyRuntime( _runtime );
            JS_ShutDown();
        }

        Scope * createScope();
        
        void runTest();
        
        virtual bool utf8Ok() const { return JS_CStringsAreUTF8(); }
        
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


    JSBool no_gc(JSContext *cx, JSGCStatus status){
        return JS_FALSE;
    }
        
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
            //JS_SetGCCallback( _context , no_gc ); // this is useful for seeing if something is a gc problem
        }

        ~SMScope(){
            uassert( "deleted SMScope twice?" , _convertor );

            for ( list<void*>::iterator i=_roots.begin(); i != _roots.end(); i++ ){
                JS_RemoveRoot( _context , *i );
            }

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
            massert( "SMScope::reset() not implemented yet" , 0 );
        }
        
        void addRoot( void * root , const char * name ){
            JS_AddNamedRoot( _context , root , name );
            _roots.push_back( root );
        }
        
        void init( BSONObj * data ){
            if ( ! data )
                return;
                
            BSONObjIterator i( *data );
            while ( i.more() ){
                BSONElement e = i.next();
                if ( e.eoo() )
                    break;
                
                _convertor->setProperty( _global , e.fieldName() , _convertor->toval( e ) );
            }

        }

        void externalSetup(){
            initMongoJS( this , _context , _global , false );
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

        void setObject( const char *field , const BSONObj& obj , bool readOnly ){
            jsval v = _convertor->toval( &obj , readOnly );
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
        
        ScriptingFunction createFunction( const char * code ){
            precall();
            return (ScriptingFunction)_convertor->compileFunction( code );
        }

        struct TimeoutSpec {
            boost::posix_time::ptime start;
            boost::posix_time::time_duration timeout;
            int count;
        };
        
        static JSBool checkTimeout( JSContext *cx, JSScript *script ) {
            TimeoutSpec &spec = *(TimeoutSpec *)( JS_GetContextPrivate( cx ) );
            if ( ++spec.count % 1000 != 0 )
                return JS_TRUE;
            boost::posix_time::time_duration elapsed = ( boost::posix_time::microsec_clock::local_time() - spec.start );
            if ( elapsed < spec.timeout ) {
                return JS_TRUE;
            }
            JS_ReportError( cx, "Timeout exceeded" );
            return JS_FALSE;
        }

        void installCheckTimeout( int timeoutMs ) {
            if ( timeoutMs > 0 ) {
                TimeoutSpec *spec = new TimeoutSpec;
                spec->timeout = boost::posix_time::millisec( timeoutMs );
                spec->start = boost::posix_time::microsec_clock::local_time();
                spec->count = 0;
                JS_SetContextPrivate( _context, (void*)spec );
                JS_SetBranchCallback( _context, checkTimeout );
            }            
        }

        void uninstallCheckTimeout( int timeoutMs ) {
            if ( timeoutMs > 0 ) {
                JS_SetBranchCallback( _context, 0 );
                delete (TimeoutSpec *)JS_GetContextPrivate( _context );
                JS_SetContextPrivate( _context, 0 );
            }
        }
        
        void precall(){
            _error = "";
            currentScope.reset( this );
        }
        
        bool exec( const string& code , const string& name = "(anon)" , bool printResult = false , bool reportError = true , bool assertOnError = true, int timeoutMs = 0 ){
            precall();
            
            jsval ret = JSVAL_VOID;
            
            installCheckTimeout( timeoutMs );
            JSBool worked = JS_EvaluateScript( _context , _global , code.c_str() , strlen( code.c_str() ) , name.c_str() , 0 , &ret );
            uninstallCheckTimeout( timeoutMs );
            
            if ( assertOnError )
                uassert( name + " exec failed" , worked );
            
            if ( reportError && ! _error.empty() ){
                // cout << "exec error: " << _error << endl;
                // already printed in reportError, so... TODO
            }
            
            if ( worked && printResult && ! JSVAL_IS_VOID( ret ) )
                cout << _convertor->toString( ret ) << endl;

            return worked;
        }

        int invoke( JSFunction * func , const BSONObj& args, int timeoutMs ){
            precall();
            jsval rval;
            
            int nargs = args.nFields();
            auto_ptr<jsval> smargsPtr( new jsval[nargs] );
            jsval* smargs = smargsPtr.get();

            BSONObjIterator it( args );
            for ( int i=0; i<nargs; i++ )
                smargs[i] = _convertor->toval( it.next() );
            
            setObject( "args" , args , true ); // this is for backwards compatability

            installCheckTimeout( timeoutMs );
            JSBool ret = JS_CallFunction( _context , _this , func , nargs , smargs , &rval );
            uninstallCheckTimeout( timeoutMs );
            
            if ( !ret ) {
                return -3;
            }

            assert( JS_SetProperty( _context , _global , "return" , &rval ) );
            return 0;
        }

        int invoke( ScriptingFunction funcAddr , const BSONObj& args, int timeoutMs = 0 ){
            return invoke( (JSFunction*)funcAddr , args , timeoutMs );
        }

        void gotError( string s ){
            _error = s;
        }
        
        string getError(){
            return _error;
        }

        void injectNative( const char *field, NativeFunction func ){
            string name = field;
            _convertor->setProperty( _global , (name + "_").c_str() , PRIVATE_TO_JSVAL( func ) );
            
            stringstream code;
            code << field << " = function(){ var a = [ " << field << "_ ]; for ( var i=0; i<arguments.length; i++ ){ a.push( arguments[i] ); } return nativeHelper.apply( null , a ); }";
            exec( code.str().c_str() );
            
        }

        virtual void gc(){
            JS_GC( _context );
        }

        JSContext *context() const { return _context; }
        
    private:
        JSContext * _context;
        Convertor * _convertor;

        JSObject * _global;
        JSObject * _this;

        string _error;
        list<void*> _roots;
    };

    void errorReporter( JSContext *cx, const char *message, JSErrorReport *report ){
        stringstream ss;
        ss << "JS Error: " << message;
        
        if ( report ){
            ss << " " << report->filename << ":" << report->lineno;
        }
        
        log() << ss.str() << endl;

        if ( currentScope.get() ){
            currentScope->gotError( ss.str() );
        }
    }

    JSBool native_load( JSContext *cx , JSObject *obj , uintN argc, jsval *argv , jsval *rval ){
        Convertor c(cx);

        Scope * s = currentScope.get();

        for ( uintN i=0; i<argc; i++ ){
            string filename = c.toString( argv[i] );
            cout << "should load [" << filename << "]" << endl;
            
            if ( ! s->execFile( filename , false , true , false ) ){
                JS_ReportError( cx , ((string)"error loading file: " + filename ).c_str() );
                return JS_FALSE;
            }
        }
        
        return JS_TRUE;
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

    void Convertor::addRoot( JSFunction * f ){
        if ( ! f )
            return;
        
        SMScope * scope = currentScope.get();
        uassert( "need a scope" , scope );
        
        scope->addRoot( f , "cf" );
    }    
    
}

#include "sm_db.cpp"
