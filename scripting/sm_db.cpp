// sm_db.cpp

// hacked in right now from engine_spidermonkey.cpp

namespace mongo {

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
            cout << "client x: " << client << endl;
            cout << "Addr in finalize: " << client->getServerAddress() << endl;
            delete client;
        }
    }

    JSClass mongo_class = {
        "Mongo" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, mongo_finalize,
        0 , 0 , 0 , 
        mongo_constructor , 
        0 , 0 , 0 , 0
    };

    JSClass mongo_local_class = {
        "Mongo" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE ,
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, JS_ResolveStub , JS_ConvertStub, mongo_finalize,
        JSCLASS_NO_OPTIONAL_MEMBERS
     };

    JSBool mongo_find(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval){
        uassert( "mongo_find not done - but yay" , 0 );
        return JS_TRUE;
    }


    JSFunctionSpec mongo_functions[] = {
        { "find" , mongo_find , 0 , 0 , JSPROP_READONLY | JSPROP_PERMANENT } ,
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
        0 , 0 , 0 , 
        object_id_constructor , 
        0 , 0 , 0 , 0
    };


}
