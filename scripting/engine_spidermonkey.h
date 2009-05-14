// engine_spidermonkey.h

#pragma once

#define JS_THREADSAFE

#include "engine.h"

#if defined( MOZJS )

#include "mozjs/jsapi.h"
#include "mozjs/jsdate.h"

#elif defined( OLDJS )

#ifdef WIN32
#include "jstypes.h"
#undef JS_PUBLIC_API
#undef JS_PUBLIC_DATA
#define JS_PUBLIC_API(t)    t
#define JS_PUBLIC_DATA(t)   t
#endif

#include "jsapi.h"
#include "jsdate.h"
#ifndef JSCLASS_GLOBAL_FLAGS
#define JSCLASS_GLOBAL_FLAGS 0
#endif

#else

#include "js/jsapi.h"
#include "js/jsdate.h"

#endif

namespace mongo {

    class SMScope;
    class Convertor;
    
    extern JSClass bson_class;
    extern JSClass bson_ro_class;
    extern JSClass object_id_class;

    // internal things
    void dontDeleteScope( SMScope * s ){}
    void errorReporter( JSContext *cx, const char *message, JSErrorReport *report );
    extern boost::thread_specific_ptr<SMScope> currentScope;
    
    // bson
    JSBool resolveBSONField( JSContext *cx, JSObject *obj, jsval id, uintN flags, JSObject **objp );


    // mongo
    void initMongoJS( SMScope * scope , JSContext * cx , JSObject * global , bool local );
    bool appendSpecialDBObject( Convertor * c , BSONObjBuilder& b , const string& name , JSObject * o );
}
