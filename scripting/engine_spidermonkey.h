// engine_spidermonkey.h

#pragma once


#include "engine.h"

#ifdef _WIN32
#define XP_WIN
#else
#define XP_UNIX
#endif

#ifdef MOZJS
#include "mozjs/jsapi.h"
#else
#include "js/jsapi.h"
#endif

namespace mongo {

    class SMScope;
    
    // internal things
    void dontDeleteScope( SMScope * s ){}
    void errorReporter( JSContext *cx, const char *message, JSErrorReport *report );
    extern boost::thread_specific_ptr<SMScope> currentScope;
    
    // bson
    JSBool resolveBSONField( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp );


    // mongo
    JSBool native_db_create( JSContext * cx , JSObject * obj , uintN argc, jsval *argv, jsval *rval );
    JSObject * doCreateCollection( JSContext * cx , JSObject * db , const string& shortName );
    extern JSClass mongo_local_class;
    extern JSClass object_id_class;

}
