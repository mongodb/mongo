// sm_db.cpp

// hacked in right now from engine_spidermonkey.cpp

namespace mongo {
    
    // ------------    some defs needed ---------------
    
    JSObject * doCreateCollection( JSContext * cx , JSObject * db , const string& shortName );
        
    // ------------     utils          ------------------
         

    bool isSpecialName( const string& name ){
        static set<string> names;
        if ( names.size() == 0 ){
            names.insert( "_mongo" );
            names.insert( "_db" );
            names.insert( "_name" );
            names.insert( "_fullName" );
            names.insert( "_shortName" );
            names.insert( "tojson" );
            names.insert( "toJson" );
            names.insert( "toString" );
        }
        
        if ( name.length() == 0 )
            return false;
        
        if ( name[0] == '_' )
            return true;
        
        return names.count( name ) > 0;
    }


    // ------ cursor ------

    class CursorHolder {
    public:
        CursorHolder( auto_ptr< DBClientCursor > &cursor, const shared_ptr< DBClientBase > &connection ) :
        connection_( connection ),
        cursor_( cursor ) {
            assert( cursor_.get() );
        }
        DBClientCursor *get() const { return cursor_.get(); }
    private:
        shared_ptr< DBClientBase > connection_;
        auto_ptr< DBClientCursor > cursor_;
    };
    
    DBClientCursor *getCursor( JSContext *cx, JSObject *obj ) {
        CursorHolder * holder = (CursorHolder*)JS_GetPrivate( cx , obj );
        uassert( "no cursor!" , holder );
        return holder->get();
    }
    
    JSBool internal_cursor_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        uassert( "no args to internal_cursor_constructor" , argc == 0 );
        assert( JS_SetPrivate( cx , obj , 0 ) ); // just for safety
        return JS_TRUE;
    }

    void internal_cursor_finalize( JSContext * cx , JSObject * obj ){
        CursorHolder * holder = (CursorHolder*)JS_GetPrivate( cx , obj );
        if ( holder ){
            delete holder;
            assert( JS_SetPrivate( cx , obj , 0 ) );
        }
    }

    JSBool internal_cursor_hasNext(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        DBClientCursor *cursor = getCursor( cx, obj );
        *rval = cursor->more() ? JSVAL_TRUE : JSVAL_FALSE;
        return JS_TRUE;
    }

    JSBool internal_cursor_next(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        DBClientCursor *cursor = getCursor( cx, obj );
        Convertor c(cx);

        BSONObj n = cursor->next();
        *rval = c.toval( &n );
        return JS_TRUE;
    }
    

    JSFunctionSpec internal_cursor_functions[] = {
        { "hasNext" , internal_cursor_hasNext , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
        { "next" , internal_cursor_next , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
        { 0 }
    };

    JSClass internal_cursor_class = {
        "InternalCursor" , JSCLASS_HAS_PRIVATE  ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, internal_cursor_finalize,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    
    // ------ mongo stuff ------

    JSBool mongo_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        uassert( "mongo_constructor not implemented yet" , 0 );
        throw -1;
    }
    
    JSBool mongo_local_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        Convertor c( cx );

        shared_ptr< DBClientBase > client( createDirectClient() );
        assert( JS_SetPrivate( cx , obj , (void*)( new shared_ptr< DBClientBase >( client ) ) ) );

        jsval host = c.toval( "EMBEDDED" );
        assert( JS_SetProperty( cx , obj , "host" , &host ) );

        return JS_TRUE;
    }

    JSBool mongo_external_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        Convertor c( cx );
        
        uassert( "0 or 1 args to Mongo" , argc <= 1 );
        
        shared_ptr< DBClientConnection > conn( new DBClientConnection( true ) );
        
        string host = "127.0.0.1";
        if ( argc > 0 )
            host = c.toString( argv[0] );

        string errmsg;
        if ( ! conn->connect( host , errmsg ) ){
            JS_ReportError( cx , ((string)"couldn't connect: " + errmsg).c_str() );
            return JS_FALSE;
        }
        
        assert( JS_SetPrivate( cx , obj , (void*)( new shared_ptr< DBClientConnection >( conn ) ) ) );
        jsval host_val = c.toval( host.c_str() );
        assert( JS_SetProperty( cx , obj , "host" , &host_val ) );
        return JS_TRUE;

    }

    DBClientBase *getConnection( JSContext *cx, JSObject *obj ) {
        shared_ptr< DBClientBase > * connHolder = (shared_ptr< DBClientBase >*)JS_GetPrivate( cx , obj );
        uassert( "no connection!" , connHolder && connHolder->get() );
        return connHolder->get();
    }
    
    void mongo_finalize( JSContext * cx , JSObject * obj ){
        shared_ptr< DBClientBase > * connHolder = (shared_ptr< DBClientBase >*)JS_GetPrivate( cx , obj );
        if ( connHolder ){
            delete connHolder;
            assert( JS_SetPrivate( cx , obj , 0 ) );
        }
    }

    JSClass mongo_class = {
        "Mongo" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, mongo_finalize,
        JSCLASS_NO_OPTIONAL_MEMBERS
     };

    JSBool mongo_find(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        uassert( "mongo_find neesd 5 args" , argc == 5 );
        shared_ptr< DBClientBase > * connHolder = (shared_ptr< DBClientBase >*)JS_GetPrivate( cx , obj );
        uassert( "no connection!" , connHolder && connHolder->get() );
        DBClientBase *conn = connHolder->get();
                      
        Convertor c( cx );

        string ns = c.toString( argv[0] );
        
        BSONObj q = c.toObject( argv[1] );
        BSONObj f = c.toObject( argv[2] );
        
        int nToReturn = (int) c.toNumber( argv[3] );
        int nToSkip = (int) c.toNumber( argv[4] );
        bool slaveOk = c.getBoolean( obj , "slaveOk" );

        try {

            auto_ptr<DBClientCursor> cursor = conn->query( ns , q , nToReturn , nToSkip , f.nFields() ? &f : 0  , slaveOk ? Option_SlaveOk : 0 );
            
            JSObject * mycursor = JS_NewObject( cx , &internal_cursor_class , 0 , 0 );
            assert( JS_SetPrivate( cx , mycursor , new CursorHolder( cursor, *connHolder ) ) );
            *rval = OBJECT_TO_JSVAL( mycursor );
            return JS_TRUE;
        }
        catch ( ... ){
            JS_ReportError( cx , "error doing query" );
            return JS_FALSE;
        }
    }

    JSBool mongo_update(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        uassert( "mongo_find needs at elast 3 args" , argc >= 3 );
        uassert( "2nd param to update has to be an object" , JSVAL_IS_OBJECT( argv[1] ) );
        uassert( "3rd param to update has to be an object" , JSVAL_IS_OBJECT( argv[2] ) );
        
        DBClientBase * conn = getConnection( cx, obj );
        uassert( "no connection!" , conn );

        Convertor c( cx );

        string ns = c.toString( argv[0] );

        bool upsert = argc > 3 && c.toBoolean( argv[3] );

        try {
            conn->update( ns , c.toObject( argv[1] ) , c.toObject( argv[2] ) , upsert );
            return JS_TRUE;
        }
        catch ( ... ){
            JS_ReportError( cx , "error doing update" );
            return JS_FALSE;
        }
    }

    JSBool mongo_insert(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){    
        uassert( "mongo_insert needs 2 args" , argc == 2 );
        uassert( "2nd param to insert has to be an object" , JSVAL_IS_OBJECT( argv[1] ) );
        
        DBClientBase * conn = getConnection( cx, obj );
        uassert( "no connection!" , conn );
        
        Convertor c( cx );
        
        string ns = c.toString( argv[0] );
        BSONObj o = c.toObject( argv[1] );

        // TODO: add _id
        
        try {
            conn->insert( ns , o );
            return JS_TRUE;
        }
        catch ( ... ){
            JS_ReportError( cx , "error doing insert" );
            return JS_FALSE;
        }
    }

    JSBool mongo_remove(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){    
        uassert( "mongo_remove needs 2 arguments" , argc == 2 );
        uassert( "2nd param to insert has to be an object" , JSVAL_IS_OBJECT( argv[1] ) );

        DBClientBase * conn = getConnection( cx, obj );
        uassert( "no connection!" , conn );

        Convertor c( cx );
        
        string ns = c.toString( argv[0] );
        BSONObj o = c.toObject( argv[1] );

        try {
            conn->remove( ns , o );
            return JS_TRUE;
        }
        catch ( ... ){
            JS_ReportError( cx , "error doing remove" );
            return JS_FALSE;
        }
        
    }

    JSFunctionSpec mongo_functions[] = {
        { "find" , mongo_find , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
        { "update" , mongo_update , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
        { "insert" , mongo_insert , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
        { "remove" , mongo_remove , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
        { 0 }
    };


     // -------------  db_collection -------------

     JSBool db_collection_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){    
         uassert( "db_collection_constructor wrong args" , argc == 4 );
         assert( JS_SetProperty( cx , obj , "_mongo" , &(argv[0]) ) );
         assert( JS_SetProperty( cx , obj , "_db" , &(argv[1]) ) );
         assert( JS_SetProperty( cx , obj , "_shortName" , &(argv[2]) ) );
         assert( JS_SetProperty( cx , obj , "_fullName" , &(argv[3]) ) );
         return JS_TRUE;
     }

     JSBool db_collection_resolve( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ){
         if ( flags & JSRESOLVE_ASSIGNING )
             return JS_TRUE;
         
         Convertor c( cx );
         string collname = c.toString( id );

         if ( isSpecialName( collname ) )
             return JS_TRUE;
         
         if ( obj == c.getGlobalPrototype( "DBCollection" ) )
             return JS_TRUE;
         
         JSObject * proto = JS_GetPrototype( cx , obj );
         if ( c.hasProperty( obj , collname.c_str() ) || ( proto && c.hasProperty( proto , collname.c_str() )  ) )
             return JS_TRUE;
         
         string name = c.toString( c.getProperty( obj , "_shortName" ) );
         name += ".";
         name += collname;
         
         jsval db = c.getProperty( obj , "_db" );
         if ( ! JSVAL_IS_OBJECT( db ) )
             return JS_TRUE;

         JSObject * coll = doCreateCollection( cx , JSVAL_TO_OBJECT( db ) , name );
         c.setProperty( obj , collname.c_str() , OBJECT_TO_JSVAL( coll ) );
         *objp = obj;
         return JS_TRUE;
     }

    JSClass db_collection_class = {
         "DBCollection" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE , 
         JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
         JS_EnumerateStub, (JSResolveOp)(&db_collection_resolve) , JS_ConvertStub, JS_FinalizeStub,
         JSCLASS_NO_OPTIONAL_MEMBERS
     };


     JSObject * doCreateCollection( JSContext * cx , JSObject * db , const string& shortName ){
         Convertor c(cx);
         
         assert( c.hasProperty( db , "_mongo" ) );
         assert( c.hasProperty( db , "_name" ) );
         
         JSObject * coll = JS_NewObject( cx , &db_collection_class , 0 , 0 );
         c.setProperty( coll , "_mongo" , c.getProperty( db , "_mongo" ) );
         c.setProperty( coll , "_db" , OBJECT_TO_JSVAL( db ) );
         c.setProperty( coll , "_shortName" , c.toval( shortName.c_str() ) );
         
         string name = c.toString( c.getProperty( db , "_name" ) );
         name += "." + shortName;
         c.setProperty( coll , "_fullName" , c.toval( name.c_str() ) );
         
         return coll;
    }
    
    // --------------  DB ---------------
    
    
    JSBool db_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        uassert( "wrong number of arguments to DB" , argc == 2 );
        assert( JS_SetProperty( cx , obj , "_mongo" , &(argv[0]) ) );
        assert( JS_SetProperty( cx , obj , "_name" , &(argv[1]) ) );

        return JS_TRUE;
    }

    JSBool db_resolve( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ){
        if ( flags & JSRESOLVE_ASSIGNING )
            return JS_TRUE;

        Convertor c( cx );

        if ( obj == c.getGlobalPrototype( "DB" ) )
            return JS_TRUE;

        string collname = c.toString( id );
        
        if ( isSpecialName( collname ) )
             return JS_TRUE;

        JSObject * proto = JS_GetPrototype( cx , obj );
        if ( proto && c.hasProperty( proto , collname.c_str() ) )
            return JS_TRUE;

        JSObject * coll = doCreateCollection( cx , obj , collname );
        c.setProperty( obj , collname.c_str() , OBJECT_TO_JSVAL( coll ) );
        
        *objp = obj;
        return JS_TRUE;
    }

    JSClass db_class = {
        "DB" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE , 
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, (JSResolveOp)(&db_resolve) , JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };
    

    // -------------- object id -------------

    JSBool object_id_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        Convertor c( cx );

        OID oid;
        if ( argc == 0 ){
            oid.init();
        }
        else {
            uassert( "object_id_constructor can't take more than 1 param" , argc == 1 );
            oid.init( c.toString( argv[0] ) );
        }
        
        if ( ! JS_InstanceOf( cx , obj , &object_id_class , 0 ) ){
            obj = JS_NewObject( cx , &object_id_class , 0 , 0 );
            assert( obj );
            *rval = OBJECT_TO_JSVAL( obj );
        }
        
        jsval v = c.toval( oid.str().c_str() );
        assert( JS_SetProperty( cx , obj , "str" , &v  ) );

        return JS_TRUE;
    }
    
    JSClass object_id_class = {
        "ObjectId" , JSCLASS_HAS_PRIVATE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    JSBool object_id_tostring(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){    
        Convertor c(cx);
        return *rval = c.getProperty( obj , "str" );
    }

    JSFunctionSpec object_id_functions[] = {
        { "toString" , object_id_tostring , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
        { 0 }
    };
    

    JSClass timestamp_class = {
        "Timestamp" , JSCLASS_HAS_PRIVATE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    JSClass minkey_class = {
        "MinKey" , JSCLASS_HAS_PRIVATE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    JSClass maxkey_class = {
        "MaxKey" , JSCLASS_HAS_PRIVATE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };
    
    // dbquery

    JSBool dbquery_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        uassert( "DDQuery needs at least 4 args" , argc >= 4 );
        
        Convertor c(cx);
        c.setProperty( obj , "_mongo" , argv[0] );
        c.setProperty( obj , "_db" , argv[1] );
        c.setProperty( obj , "_collection" , argv[2] );
        c.setProperty( obj , "_ns" , argv[3] );

        if ( argc > 4 && JSVAL_IS_OBJECT( argv[4] ) )
            c.setProperty( obj , "_query" , argv[4] );
        else 
            c.setProperty( obj , "_query" , OBJECT_TO_JSVAL( JS_NewObject( cx , 0 , 0 , 0 ) ) );
        
        if ( argc > 5 && JSVAL_IS_OBJECT( argv[5] ) )
            c.setProperty( obj , "_fields" , argv[5] );
        else
            c.setProperty( obj , "_fields" , JSVAL_NULL );
        
        
        if ( argc > 6 && JSVAL_IS_NUMBER( argv[6] ) )
            c.setProperty( obj , "_limit" , argv[6] );
        else 
            c.setProperty( obj , "_limit" , JSVAL_ZERO );
        
        if ( argc > 7 && JSVAL_IS_NUMBER( argv[7] ) )
            c.setProperty( obj , "_skip" , argv[7] );
        else 
            c.setProperty( obj , "_skip" , JSVAL_ZERO );
        
        c.setProperty( obj , "_cursor" , JSVAL_NULL );
        c.setProperty( obj , "_numReturned" , JSVAL_ZERO );
        c.setProperty( obj , "_special" , JSVAL_FALSE );

        return JS_TRUE;
    }

    JSBool dbquery_resolve( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ){
        if ( flags & JSRESOLVE_ASSIGNING )
            return JS_TRUE;

        if ( ! JSVAL_IS_NUMBER( id ) )
            return JS_TRUE;

        jsval val = JSVAL_VOID;
        assert( JS_CallFunctionName( cx , obj , "arrayAccess" , 1 , &id , &val ) );
        Convertor c(cx);
        c.setProperty( obj , c.toString( id ).c_str() , val );
        *objp = obj;
        return JS_TRUE;
    }

    JSClass dbquery_class = {
        "DBQuery" , JSCLASS_NEW_RESOLVE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, (JSResolveOp)(&dbquery_resolve) , JS_ConvertStub, JS_FinalizeStub,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };
    
    // ---- other stuff ----
    
    void initMongoJS( SMScope * scope , JSContext * cx , JSObject * global , bool local ){

        assert( JS_InitClass( cx , global , 0 , &mongo_class , local ? mongo_local_constructor : mongo_external_constructor , 0 , 0 , mongo_functions , 0 , 0 ) );
        
        assert( JS_InitClass( cx , global , 0 , &object_id_class , object_id_constructor , 0 , 0 , object_id_functions , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &db_class , db_constructor , 2 , 0 , 0 , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &db_collection_class , db_collection_constructor , 4 , 0 , 0 , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &internal_cursor_class , internal_cursor_constructor , 0 , 0 , internal_cursor_functions , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &dbquery_class , dbquery_constructor , 0 , 0 , 0 , 0 , 0 ) );

        assert( JS_InitClass( cx , global , 0 , &timestamp_class , 0 , 0 , 0 , 0 , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &minkey_class , 0 , 0 , 0 , 0 , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &maxkey_class , 0 , 0 , 0 , 0 , 0 , 0 ) );
        
        scope->exec( jsconcatcode );
    }

    bool appendSpecialDBObject( Convertor * c , BSONObjBuilder& b , const string& name , JSObject * o ){

        if ( JS_InstanceOf( c->_context , o , &object_id_class , 0 ) ){
            OID oid;
            oid.init( c->getString( o , "str" ) );
            b.append( name.c_str() , oid );
            return true;
        }

        if ( JS_InstanceOf( c->_context , o , &minkey_class , 0 ) ){
            b.appendMinKey( name.c_str() );
            return true;
        }

        if ( JS_InstanceOf( c->_context , o , &maxkey_class , 0 ) ){
            b.appendMaxKey( name.c_str() );
            return true;
        }
        
        if ( JS_InstanceOf( c->_context , o , &timestamp_class , 0 ) ){
            b.appendTimestamp( name.c_str() , (unsigned long long)c->getNumber( o , "t" ) , (unsigned int )c->getNumber( o , "i" ) );
            return true;
        }
        
#ifdef SM16
        {
            jsdouble d = js_DateGetMsecSinceEpoch( c->_context , o );
            if ( d ){
                b.appendDate( name.c_str() , (unsigned long long)d );
                return true;
            }
        }
#else
        if ( JS_InstanceOf( c->_context , o, &js_DateClass, 0 ) ){
            jsdouble d = js_DateGetMsecSinceEpoch( c->_context , o );
            b.appendDate( name.c_str() , (unsigned long long)d );
            return true;
        }
#endif


        return false;
    }

}
