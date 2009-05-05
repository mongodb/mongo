// sm_db.cpp

// hacked in right now from engine_spidermonkey.cpp

namespace mongo {

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
         0 , 0 , 0 , 
         mongo_local_constructor , 
         0 , 0 , 0 , 0
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
         Convertor c( cx );
         string collname = c.toString( id );
         
         if ( collname == "_mongo" || collname == "_db" || collname == "_shortName" || collname == "_fullName" )
             return JS_TRUE;
         
         JSObject * proto = JS_GetPrototype( cx , obj );
         if ( proto && c.hasProperty( proto , collname.c_str() ) )
             return JS_TRUE;
         
         string name = c.toString( c.getProperty( obj , "_shortName" ) );
         name += ".";
         name += collname;
         
         JSObject * coll = doCreateCollection( cx , JSVAL_TO_OBJECT( c.getProperty( obj , "_db" ) ) , name );
         c.setProperty( obj , collname.c_str() , OBJECT_TO_JSVAL( coll ) );
         *objp = obj;
         return JS_TRUE;
     }

     static JSClass db_collection_class = {
         "DB" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE , 
         JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
         JS_EnumerateStub, (JSResolveOp)(&db_collection_resolve) , JS_ConvertStub, JS_FinalizeStub,
         0 , 0 , 0 , 
         db_collection_constructor , 
         0 , 0 , 0 , 0        
     };


     JSObject * doCreateCollection( JSContext * cx , JSObject * db , const string& shortName ){
         Convertor c(cx);

         JSObject * coll = JS_NewObject( cx , &db_collection_class , 0 , 0 );
         c.setProperty( coll , "_mongo" , c.getProperty( db , "_mongo" ) );
         c.setProperty( coll , "_db" , OBJECT_TO_JSVAL( db ) );
         c.setProperty( coll , "_shortName" , c.toval( shortName.c_str() ) );

         string name = c.toString( c.getProperty( db , "_name" ) );
         name += "." + shortName;
         c.setProperty( coll , "_fullName" , c.toval( name.c_str() ) );

        assert( JS_SetPrototype( cx , coll , c.getGlobalPrototype( "DBCollection" ) ) );
        
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
        Convertor c( cx );
        string collname = c.toString( id );

        if ( collname == "prototype" || collname.find( "__" ) == 0 || 
             collname == "_mongo" || collname == "_name" )
            return JS_TRUE;
        
        JSObject * proto = JS_GetPrototype( cx , obj );
        if ( proto && c.hasProperty( proto , collname.c_str() ) )
            return JS_TRUE;
        
        JSObject * coll = doCreateCollection( cx , obj , collname );
        c.setProperty( obj , collname.c_str() , OBJECT_TO_JSVAL( coll ) );
        
        *objp = obj;
        return JS_TRUE;
    }

    static JSClass db_class = {
        "DB" , JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE , 
        JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
        JS_EnumerateStub, (JSResolveOp)(&db_resolve) , JS_ConvertStub, JS_FinalizeStub,
        0 , 0 , 0 , 
        db_constructor , 
        0 , 0 , 0 , 0        
    };
    
    JSBool native_db_create( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval ){
        Convertor c( cx );
        uassert( "dbCreate needs 2 args" , argc == 2 );

        JSObject * proto = c.getGlobalPrototype( "DB" );
        uassert( "can't find DB prototype" , proto );
        
        JSObject * db = JS_NewObject( cx , &db_class , 0 , 0 );
        assert( JS_SetProperty( cx , db , "_mongo" , &(argv[0]) ) );
        assert( JS_SetProperty( cx , db , "_name" , &(argv[1]) ) );
        assert( JS_SetPrototype( cx , db , proto ) );
        
        *rval = OBJECT_TO_JSVAL( db );
        return JS_TRUE;
    }

    
    JSBool native_db_collection_create( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval ){
        Convertor c( cx );
        uassert( "native_db_collection_create needs 4 args" , argc == 4 );        
        
        JSObject * proto = c.getGlobalPrototype( "DBCollection" );
        uassert( "can't find DBCollection prototype" , proto );
        
        JSObject * db = JS_NewObject( cx , &db_collection_class , 0 , 0 );
        assert( JS_SetProperty( cx , obj , "_mongo" , &(argv[0]) ) );
        assert( JS_SetProperty( cx , obj , "_db" , &(argv[1]) ) );
        assert( JS_SetProperty( cx , obj , "_shortName" , &(argv[2]) ) );
        assert( JS_SetProperty( cx , obj , "_fullName" , &(argv[3]) ) );

        assert( JS_SetPrototype( cx , db , proto ) );
        
        *rval = OBJECT_TO_JSVAL( db );
        return JS_TRUE;
        
    }


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
