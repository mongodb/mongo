// MongoJS.h

#pragma once

#include <v8.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "mongo/client/dbclient.h"

void installMongoGlobals( v8::Handle<v8::ObjectTemplate>& global );

// the actual globals
v8::Handle<v8::Value> mongoInject(const v8::Arguments& args);

// utils
v8::Local<v8::Object> mongoToV8( mongo::BSONObj & m , bool array = 0 );
mongo::BSONObj v8ToMongo( v8::Handle<v8::Object> o );

mongo::DBClientConnection * getConnection( const v8::Arguments& args );



// Mongo members
v8::Handle<v8::Value> mongoInit(const v8::Arguments& args);
v8::Handle<v8::Value> mongoFind(const v8::Arguments& args);
v8::Handle<v8::Value> mongoInsert(const v8::Arguments& args);
v8::Handle<v8::Value> mongoRemove(const v8::Arguments& args);
v8::Handle<v8::Value> mongoUpdate(const v8::Arguments& args);


v8::Handle<v8::Value> internalCursorCons(const v8::Arguments& args);
v8::Handle<v8::Value> internalCursorNext(const v8::Arguments& args);
v8::Handle<v8::Value> internalCursorHasNext(const v8::Arguments& args);

// DB members

v8::Handle<v8::Value> dbInit(const v8::Arguments& args);
v8::Handle<v8::Value> collectionInit( const v8::Arguments& args );
v8::Handle<v8::Value> objectIdInit( const v8::Arguments& args );

v8::Handle<v8::Value> dbQueryInit( const v8::Arguments& args );
v8::Handle<v8::Value> dbQueryIndexAccess( uint32_t index , const v8::AccessorInfo& info );

v8::Handle<v8::Value> collectionFallback( v8::Local<v8::String> name, const v8::AccessorInfo &info);

v8::Function * getNamedCons( const char * name );
v8::Function * getObjectIdCons();
