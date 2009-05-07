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
        }
        return names.count( name ) > 0;
    }


    // ------ cursor ------

    JSBool internal_cursor_constructor( JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval ){
        uassert( "no args to internal_cursor_constructor" , argc == 0 );
        JS_SetPrivate( cx , obj , 0 ); // just for safety
        return JS_TRUE;
    }


    void internal_cursor_finalize( JSContext * cx , JSObject * obj ){
        DBClientCursor * cursor = (DBClientCursor*)JS_GetPrivate( cx , obj );
        if ( cursor ){
            delete cursor;
            JS_SetPrivate( cx , obj , 0 );
        }
    }

    JSBool internal_cursor_hasNext(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        DBClientCursor * cursor = (DBClientCursor*)JS_GetPrivate( cx , obj );
        uassert( "no cursor in hasNext!" , cursor );
        *rval = cursor->more() ? JSVAL_TRUE : JSVAL_FALSE;
        return JS_TRUE;
    }

    JSBool internal_cursor_next(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        DBClientCursor * cursor = (DBClientCursor*)JS_GetPrivate( cx , obj );
        uassert( "no cursor in next!" , cursor );

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

        DBClientBase * client = createDirectClient();
        JS_SetPrivate( cx , obj , (void*)client );

        jsval host = c.toval( "EMBEDDED" );
        assert( JS_SetProperty( cx , obj , "host" , &host ) );

        return JS_TRUE;
    }

    void mongo_finalize( JSContext * cx , JSObject * obj ){
        DBClientBase * client = (DBClientBase*)JS_GetPrivate( cx , obj );
        if ( client ){
            delete client;
            JS_SetPrivate( cx , obj , 0 );
        }
    }

    JSClass mongo_class = {
        "Mongo" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, mongo_finalize,
        JSCLASS_NO_OPTIONAL_MEMBERS
    };

    JSClass mongo_local_class = {
        "Mongo" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, mongo_finalize,
        JSCLASS_NO_OPTIONAL_MEMBERS
     };

    JSBool mongo_find(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        uassert( "mongo_find neesd 5 args" , argc == 5 );
        DBClientConnection * conn = (DBClientConnection*)JS_GetPrivate( cx , obj );
        uassert( "no connection!" , conn );

        Convertor c( cx );

        string ns = c.toString( argv[0] );
        
        BSONObj q = c.toObject( argv[1] );
        //uassert( "field selector not supported yet in mongo_find" , argv[2] == JSVAL_NULL );
        
        int nToReturn = c.toNumber( argv[3] );
        int nToSkip = c.toNumber( argv[4] );
        bool slaveOk = c.getBoolean( obj , "slaveOk" );

        try {

            auto_ptr<DBClientCursor> cursor = conn->query( ns , q , nToReturn , nToSkip , 0 , slaveOk ? Option_SlaveOk : 0 );
            
            JSObject * mycursor = JS_NewObject( cx , &internal_cursor_class , 0 , 0 );
            JS_SetPrivate( cx , mycursor , cursor.release() );
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
        
        DBClientConnection * conn = (DBClientConnection*)JS_GetPrivate( cx , obj );
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
        
        DBClientConnection * conn = (DBClientConnection*)JS_GetPrivate( cx , obj );
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

        DBClientConnection * conn = (DBClientConnection*)JS_GetPrivate( cx , obj );
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
         if ( flags & JSRESOLVE_ASSIGNING || flags & JSRESOLVE_DETECTING )
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
        if ( flags & JSRESOLVE_ASSIGNING || flags & JSRESOLVE_DETECTING )
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

        OID oid;
        if ( argc == 0 ){
            oid.init();
        }
        else {
            uassert( "object_id_constructor 2nd case" , 0 );
        }
        
        Convertor c( cx );
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
    

    // ---- other stuff ----

    void initMongoJS( SMScope * scope , JSContext * cx , JSObject * global , bool local ){
        uassert( "non-local not supported yet" , local );

        assert( JS_InitClass( cx , global , 0 , &mongo_local_class , mongo_local_constructor , 0 , 0 , mongo_functions , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &object_id_class , 0 , 0 , 0 , 0 , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &db_class , db_constructor , 2 , 0 , 0 , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &db_collection_class , db_collection_constructor , 4 , 0 , 0 , 0 , 0 ) );
        assert( JS_InitClass( cx , global , 0 , &internal_cursor_class , internal_cursor_constructor , 0 , 0 , internal_cursor_functions , 0 , 0 ) );

        scope->exec( jsconcatcode );

    }

}
