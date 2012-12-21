//engine_v8.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <v8.h>
#include <vector>

#include "mongo/scripting/engine.h"

#include "mongo/base/disallow_copying.h"

using namespace v8;

/**
 * V8_SIMPLE_HEADER must be placed in any function called from a public API
 * that work with v8 handles (and/or must be within the V8Scope's isolate
 * and context.
 */
#define V8_SIMPLE_HEADER                                                                         \
        v8::Locker v8lock(_isolate);          /* acquire isolate lock */                         \
        v8::Isolate::Scope iscope(_isolate);  /* enter the isolate and exit when out of scope */ \
        HandleScope handle_scope;             /* make the current scope own local handles */     \
        Context::Scope context_scope(_context); /* enter the context and exit when out of scope */

namespace mongo {

    class V8ScriptEngine;
    class V8Scope;

    typedef Handle< Value > (*v8Function) ( V8Scope* scope, const v8::Arguments& args );

    class BSONHolder {
    public:

        BSONHolder( BSONObj obj ) {
            _obj = obj.getOwned();
            _modified = false;
            v8::V8::AdjustAmountOfExternalAllocatedMemory(_obj.objsize());
        }

        ~BSONHolder() {
            v8::V8::AdjustAmountOfExternalAllocatedMemory(-_obj.objsize());
        }

        BSONObj _obj;
        bool _modified;
        list<string> _extra;
        set<string> _removed;

    };

    /**
     * A V8Scope represents a unit of javascript execution environment; specifically a single
     * isolate and a single context executing in a single mongo thread.  A V8Scope can be reused
     * in another thread only after reset() has been called.
     *
     * NB:
     *   - v8 objects/handles/etc. cannot be shared between V8Scopes
     *   - in mongod, each scope is associated with an opId (for KillOp support)
     *   - any public functions that call the v8 API should have a V8_SIMPLE_HEADER
     */
    class V8Scope : public Scope {
    public:

        V8Scope( V8ScriptEngine * engine );
        ~V8Scope();

        virtual void init( const BSONObj * data );

        /**
         * Reset the state of this scope for use by another thread or operation
         */
        virtual void reset();

        /**
         * Terminate this scope
         */
        virtual void kill();

        virtual void localConnect( const char * dbName );
        virtual void externalSetup();

        v8::Handle<v8::Value> get( const char * field ); // caller must create context and handle scopes
        virtual double getNumber( const char *field );
        virtual int getNumberInt( const char *field );
        virtual long long getNumberLongLong( const char *field );
        virtual string getString( const char *field );
        virtual bool getBoolean( const char *field );
        virtual BSONObj getObject( const char *field );

        virtual int type( const char *field );

        virtual void setNumber( const char *field , double val );
        virtual void setString( const char *field , const char * val );
        virtual void setBoolean( const char *field , bool val );
        virtual void setElement( const char *field , const BSONElement& e );
        virtual void setObject( const char *field , const BSONObj& obj , bool readOnly);
        virtual void setFunction( const char *field , const char * code );

        virtual void rename( const char * from , const char * to );

        virtual ScriptingFunction _createFunction( const char * code );
        Local< v8::Function > __createFunction( const char * code );
        virtual int invoke( ScriptingFunction func , const BSONObj* args, const BSONObj* recv, int timeoutMs = 0 , bool ignoreReturn = false, bool readOnlyArgs = false, bool readOnlyRecv = false );
        virtual bool exec( const StringData& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs );
        virtual string getError() { return _error; }
        virtual bool hasOutOfMemoryException();

        // functions to create v8 object and function templates
        virtual void injectNative( const char *field, NativeFunction func, void* data = 0 );
        void injectNative( const char *field, NativeFunction func, Handle<v8::Object>& obj, void* data = 0 );
        void injectV8Function( const char *field, v8Function func );
        void injectV8Function( const char *field, v8Function func, Handle<v8::Object>& obj );
        void injectV8Function( const char *field, v8Function func, Handle<v8::Template>& t );
        Handle<v8::FunctionTemplate> createV8Function( v8Function func );

        /**
         * Run the garbage collector on this scope (native function).  @see GCV8 for the
         * javascript binding version.
         */
        void gc();

        Handle< Context > context() const { return _context; }

        /**
         * Convert BSON types to v8 Javascript types
         */
        v8::Local<v8::Object> mongoToV8(const mongo::BSONObj& m, bool array = 0,
                                        bool readOnly = false);
        v8::Handle<v8::Object> mongoToLZV8(const mongo::BSONObj& m, bool readOnly = false);
        v8::Handle<v8::Value> mongoToV8Element( const BSONElement &f, bool readOnly = false );

        /**
         * Convert v8 Javascript types to BSON types
         */
        mongo::BSONObj v8ToMongo(v8::Handle<v8::Object> obj, int depth = 0);
        void v8ToMongoElement(BSONObjBuilder& b,
                              const string& sname,
                              v8::Handle<v8::Value> value,
                              int depth = 0,
                              BSONObj* originalParent = 0);
        void v8ToMongoObject(BSONObjBuilder& b,
                             const string& sname,
                             v8::Handle<v8::Value> value,
                             int depth,
                             BSONObj* originalParent);
        void v8ToMongoNumber(BSONObjBuilder& b,
                             const string& elementName,
                             v8::Handle<v8::Value> value,
                             BSONObj* originalParent);
        void v8ToMongoNumberLong(BSONObjBuilder& b,
                                 const string& elementName,
                                 v8::Handle<v8::Object> obj);
        void v8ToMongoInternal(BSONObjBuilder& b,
                               const string& elementName,
                               v8::Handle<v8::Object> obj);
        void v8ToMongoRegex(BSONObjBuilder& b,
                            const string& elementName,
                            string& regex);
        void v8ToMongoDBRef(BSONObjBuilder& b,
                            const string& elementName,
                            v8::Handle<v8::Object> obj);
        void v8ToMongoBinData(BSONObjBuilder& b,
                              const string& elementName,
                              v8::Handle<v8::Object> obj);
        void v8ToMongoObjectID(BSONObjBuilder& b,
                               const string& elementName,
                               v8::Handle<v8::Object> obj);

        v8::Function * getNamedCons( const char * name );
        v8::Function * getObjectIdCons();
        Local< v8::Value > newId( const OID &id );

        /**
         * GC callback for weak references to BSON objects (via BSONHolder)
         */
        Persistent<v8::Object> wrapBSONObject(Local<v8::Object> obj, BSONHolder* data);
        Persistent<v8::Object> wrapArrayObject(Local<v8::Object> obj, char* data);

        /**
         * Get a V8 string from the scope's cache, creating one if needed (NOTE: this may be
         * dangerous due to use in multiple threads without changing the v8Locker)
         */
        v8::Handle<v8::String> getV8Str(string str);

        /**
         * Create a V8 string with a local handle
         */
        inline v8::Handle<v8::String> getLocalV8Str(string str) { return v8::String::New(str.c_str()); }

        /**
         * Get the isolate this scope belongs to (can be called from any thread, but v8 imposes
         * some restrictions on use that requires acquiring thev8::Locker for the isolate.
         */
        v8::Isolate* getIsolate() { return _isolate; }

        /**
         * Get the JS context this scope executes within.
         */
        Persistent<Context> getContext() { return _context; }

        /**
         * Static v8 strings for function identifiers
         */
        Handle<v8::String> V8STR_CONN;
        Handle<v8::String> V8STR_ID;
        Handle<v8::String> V8STR_LENGTH;
        Handle<v8::String> V8STR_LEN;
        Handle<v8::String> V8STR_TYPE;
        Handle<v8::String> V8STR_ISOBJECTID;
        Handle<v8::String> V8STR_NATIVE_FUNC;
        Handle<v8::String> V8STR_NATIVE_DATA;
        Handle<v8::String> V8STR_V8_FUNC;
        Handle<v8::String> V8STR_RETURN;
        Handle<v8::String> V8STR_ARGS;
        Handle<v8::String> V8STR_T;
        Handle<v8::String> V8STR_I;
        Handle<v8::String> V8STR_EMPTY;
        Handle<v8::String> V8STR_MINKEY;
        Handle<v8::String> V8STR_MAXKEY;
        Handle<v8::String> V8STR_NUMBERLONG;
        Handle<v8::String> V8STR_NUMBERINT;
        Handle<v8::String> V8STR_DBPTR;
        Handle<v8::String> V8STR_BINDATA;
        Handle<v8::String> V8STR_WRAPPER;
        Handle<v8::String> V8STR_RO;
        Handle<v8::String> V8STR_FULLNAME;
        Handle<v8::String> V8STR_BSON;

    private:

        /**
         * Trampoline to call a c++ function with a specific signature (V8Scope*, v8::Arguments&).
         * Handles interruption, exceptions, etc.
         */
        static v8::Handle<v8::Value> v8Callback(const v8::Arguments& args);

        /**
         * Interpreter agnostic 'Native Callback' trampoline.  Note this is only called
         * from v8Callback().
         */
        static Handle<Value> nativeCallback(V8Scope* scope, const Arguments &args);

        /**
         * v8-specific implementations of basic global functions
         */
        static Handle<Value> load(V8Scope* scope, const Arguments &args);
        static Handle<Value> Print(V8Scope* scope, const v8::Arguments& args);
        static Handle<Value> Version(V8Scope* scope, const v8::Arguments& args);
        static Handle<Value> GCV8(V8Scope* scope, const v8::Arguments& args);

        /** Signal that this scope has entered a native (C++) execution context.
         *  @return  false if execution has been interrupted
         */
        bool nativePrologue();

        /** Signal that this scope has completed native execution and is returning to v8.
         *  @return  false if execution has been interrupted
         */
        bool nativeEpilogue();

        /**
         * Register this scope with the mongo op id.  If executing outside the
         * context of a mongo operation (e.g. from the shell), killOp will not
         * be supported.
         */
        void registerOpId();

        /**
         * Unregister this scope with the mongo op id.
         */
        void unregisterOpId();

        V8ScriptEngine * _engine;

        Persistent<Context> _context;
        Persistent<v8::Object> _global;
        string _error;
        vector< Persistent<Value> > _funcs;

        enum ConnectState { NOT , LOCAL , EXTERNAL };
        ConnectState _connectState;

        std::map <string, v8::Persistent <v8::String> > _strCache;

        Persistent<v8::FunctionTemplate> lzFunctionTemplate;
        Persistent<v8::ObjectTemplate> lzObjectTemplate;
        Persistent<v8::ObjectTemplate> roObjectTemplate;
        Persistent<v8::ObjectTemplate> lzArrayTemplate;
        Persistent<v8::ObjectTemplate> internalFieldObjects;

        v8::Isolate* _isolate;

        mongo::mutex _interruptLock; // protects interruption-related flags
        bool _inNativeExecution;     // protected by _interruptLock
        bool _pendingKill;           // protected by _interruptLock
        int _opId;                   // op id for this scope
    };

    class V8ScriptEngine : public ScriptEngine {
    public:
        V8ScriptEngine();
        virtual ~V8ScriptEngine();
        virtual Scope * createScope() { return new V8Scope( this ); }
        virtual void runTest() {}
        bool utf8Ok() const { return true; }

        /**
         * Interrupt a single active v8 execution context
         * NB: To interrupt a context, we must acquire the following locks (in order):
         *       - mutex to protect the the map of all scopes (_globalInterruptLock)
         *       - mutex to protect the scope that's being interrupted (_interruptLock)
         * The scope will be removed from the map upon destruction, and the op id
         * will be updated if the scope is ever reused from a pool.
         */
        virtual void interrupt(unsigned opId);

        /**
         * Interrupt all v8 contexts (and isolates).  @see interrupt().
         */
        virtual void interruptAll();

    private:
        friend class V8Scope;

        std::string printKnownOps_inlock();

        typedef map<unsigned, V8Scope*> OpIdToScopeMap;
        mongo::mutex _globalInterruptLock;  // protects map of all operation ids -> scope
        OpIdToScopeMap _opToScopeMap;       // map of mongo op ids to scopes (protected by
                                            // _globalInterruptLock).
    };

    extern ScriptEngine * globalScriptEngine;

}
