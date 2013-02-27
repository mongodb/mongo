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

#include "mongo/base/disallow_copying.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/v8_deadline_monitor.h"
#include "mongo/scripting/v8_profiler.h"

/**
 * V8_SIMPLE_HEADER must be placed in any function called from a public API
 * that work with v8 handles (and/or must be within the V8Scope's isolate
 * and context).  Be sure to close the handle_scope if returning a v8::Handle!
 */
#define V8_SIMPLE_HEADER                                                                      \
        v8::Locker v8lock(_isolate);          /* acquire isolate lock */                      \
        v8::Isolate::Scope iscope(_isolate);  /* enter the isolate; exit when out of scope */ \
        v8::HandleScope handle_scope;         /* make the current scope own local handles */  \
        v8::Context::Scope context_scope(_context); /* enter the context; exit when out of scope */

namespace mongo {

    class V8ScriptEngine;
    class V8Scope;

    typedef v8::Handle<v8::Value> (*v8Function)(V8Scope* scope, const v8::Arguments& args);

    /**
     * v8 callback for persistent handles that are to be freed by the GC
     */
    template <typename _T>
    void deleteOnCollect(v8::Persistent<v8::Value> objHandle, void* obj) {
        delete static_cast<_T*>(obj);
        objHandle.Dispose();
    }

    class BSONHolder {
    MONGO_DISALLOW_COPYING(BSONHolder);
    public:
        explicit BSONHolder(BSONObj obj) :
            _obj(obj.getOwned()),
            _modified(false),
            _notifyV8Memory(true) {
            // give hint v8's GC
            v8::V8::AdjustAmountOfExternalAllocatedMemory(_obj.objsize());
        }
        ~BSONHolder() {
            if (_notifyV8Memory)
                // if v8 is still up, send hint to GC
                v8::V8::AdjustAmountOfExternalAllocatedMemory(-_obj.objsize());
        }
        V8Scope* _scope;
        BSONObj _obj;
        bool _modified;
        bool _notifyV8Memory; // set to true if v8 is still usable during destruction
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
     *   - any public functions that call the v8 API should use a V8_SIMPLE_HEADER
     *   - the caller of any public function that returns a v8 type or has a v8 handle argument
     *         must enter the isolate, context, and set up the appropriate handle scope
     */
    class V8Scope : public Scope {
    public:

        V8Scope(V8ScriptEngine* engine);
        ~V8Scope();

        virtual void init(const BSONObj* data);

        /**
         * Reset the state of this scope for use by another thread or operation
         */
        virtual void reset();

        /**
         * Terminate this scope
         */
        virtual void kill();

        /** check if there is a pending killOp request */
        bool isKillPending() const;

        /**
         * Connect to a local database, create a Mongo object instance, and load any
         * server-side js into the global object
         */
        virtual void localConnect(const char* dbName);

        virtual void externalSetup();

        virtual void installDBAccess();

        virtual void installBSONTypes();

        virtual string getError() { return _error; }

        virtual bool hasOutOfMemoryException();

        /**
         * Run the garbage collector on this scope (native function).  @see GCV8 for the
         * javascript binding version.
         */
        void gc();

        /**
         * get a global property.  caller must set up the v8 state.
         */
        v8::Handle<v8::Value> get(const char* field);

        virtual double getNumber(const char* field);
        virtual int getNumberInt(const char* field);
        virtual long long getNumberLongLong(const char* field);
        virtual string getString(const char* field);
        virtual bool getBoolean(const char* field);
        virtual BSONObj getObject(const char* field);

        virtual void setNumber(const char* field, double val);
        virtual void setString(const char* field, const char* val);
        virtual void setBoolean(const char* field, bool val);
        virtual void setElement(const char* field, const BSONElement& e);
        virtual void setObject(const char* field, const BSONObj& obj, bool readOnly);
        virtual void setFunction(const char* field, const char* code);

        virtual int type(const char* field);

        virtual void rename(const char* from, const char* to);

        virtual int invoke(ScriptingFunction func, const BSONObj* args, const BSONObj* recv,
                           int timeoutMs = 0, bool ignoreReturn = false,
                           bool readOnlyArgs = false, bool readOnlyRecv = false);

        virtual bool exec(const StringData& code, const string& name, bool printResult,
                          bool reportError, bool assertOnError, int timeoutMs);

        // functions to create v8 object and function templates
        virtual void injectNative(const char* field, NativeFunction func, void* data = 0);
        void injectNative(const char* field, NativeFunction func, v8::Handle<v8::Object>& obj,
                          void* data = 0);
        void injectV8Function(const char* field, v8Function func);
        void injectV8Function(const char* field, v8Function func, v8::Handle<v8::Object>& obj);
        void injectV8Function(const char* field, v8Function func, v8::Handle<v8::Template>& t);
        v8::Handle<v8::FunctionTemplate> createV8Function(v8Function func);
        virtual ScriptingFunction _createFunction(const char* code,
                                                  ScriptingFunction functionNumber = 0);
        v8::Local<v8::Function> __createFunction(const char* code,
                                                 ScriptingFunction functionNumber = 0);

        /**
         * Convert BSON types to v8 Javascript types
         */
        v8::Local<v8::Object> mongoToV8(const mongo::BSONObj& m, bool array = 0,
                                        bool readOnly = false);
        v8::Persistent<v8::Object> mongoToLZV8(const mongo::BSONObj& m, bool readOnly = false);
        v8::Handle<v8::Value> mongoToV8Element(const BSONElement& f, bool readOnly = false);

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
                            v8::Handle<v8::Object> v8Regex);
        void v8ToMongoDBRef(BSONObjBuilder& b,
                            const string& elementName,
                            v8::Handle<v8::Object> obj);
        void v8ToMongoBinData(BSONObjBuilder& b,
                              const string& elementName,
                              v8::Handle<v8::Object> obj);
        void v8ToMongoObjectID(BSONObjBuilder& b,
                               const string& elementName,
                               v8::Handle<v8::Object> obj);

        v8::Function* getNamedCons(const char* name);

        v8::Function* getObjectIdCons();

        v8::Local<v8::Value> newId(const OID& id);

        /**
         * Convert a JavaScript exception to a stl string.  Requires
         * access to the V8Scope instance to report source context information.
         */
        std::string v8ExceptionToSTLString(const v8::TryCatch* try_catch);

        /**
         * GC callback for weak references to BSON objects (via BSONHolder)
         */
        v8::Persistent<v8::Object> wrapBSONObject(v8::Local<v8::Object> obj, BSONHolder* data);

        /**
         * Create a V8 string with a local handle
         */
        static inline v8::Handle<v8::String> v8StringData(StringData str) {
            return v8::String::New(str.rawData());
        }

        /**
         * Get the isolate this scope belongs to (can be called from any thread, but v8 requires
         *  the new thread enter the isolate and context.  Only one thread can enter the isolate.
         */
        v8::Isolate* getIsolate() { return _isolate; }

        /**
         * Get the JS context this scope executes within.
         */
        v8::Persistent<v8::Context> getContext() { return _context; }

        // store a pointer to all BSONHolder instances created in this V8Scope
        set<BSONHolder*> _lazyObjects;

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
        static v8::Handle<v8::Value> nativeCallback(V8Scope* scope, const v8::Arguments& args);

        /**
         * v8-specific implementations of basic global functions
         */
        static v8::Handle<v8::Value> load(V8Scope* scope, const v8::Arguments& args);
        static v8::Handle<v8::Value> Print(V8Scope* scope, const v8::Arguments& args);
        static v8::Handle<v8::Value> Version(V8Scope* scope, const v8::Arguments& args);
        static v8::Handle<v8::Value> GCV8(V8Scope* scope, const v8::Arguments& args);

        static v8::Handle<v8::Value> startCpuProfiler(V8Scope* scope, const v8::Arguments& args);
        static v8::Handle<v8::Value> stopCpuProfiler(V8Scope* scope, const v8::Arguments& args);
        static v8::Handle<v8::Value> getCpuProfile(V8Scope* scope, const v8::Arguments& args);

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

        /**
        * Creates a new instance of the MinKey object
        */
        v8::Local<v8::Object> newMinKeyInstance();

        /**
         * Creates a new instance of the MaxKey object
         */
        v8::Local<v8::Object> newMaxKeyInstance();

        /**
         * Create a new function; primarily used for BSON/V8 conversion.
         */
        v8::Local<v8::Value> newFunction(const char *code);

        template <typename _HandleType>
        bool checkV8ErrorState(const _HandleType& resultHandle,
                               const v8::TryCatch& try_catch,
                               bool reportError = true,
                               bool assertOnError = true);

        V8ScriptEngine* _engine;

        v8::Persistent<v8::Context> _context;
        v8::Persistent<v8::Object> _global;
        string _error;
        vector<v8::Persistent<v8::Value> > _funcs;

        enum ConnectState { NOT, LOCAL, EXTERNAL };
        ConnectState _connectState;

        v8::Persistent<v8::FunctionTemplate> lzFunctionTemplate;
        v8::Persistent<v8::ObjectTemplate> lzObjectTemplate;
        v8::Persistent<v8::ObjectTemplate> roObjectTemplate;
        v8::Persistent<v8::ObjectTemplate> lzArrayTemplate;
        v8::Persistent<v8::ObjectTemplate> internalFieldObjects;

        v8::Isolate* _isolate;
        V8CpuProfiler _cpuProfiler;

        mongo::mutex _interruptLock; // protects interruption-related flags
        bool _inNativeExecution;     // protected by _interruptLock
        bool _pendingKill;           // protected by _interruptLock
        int _opId;                   // op id for this scope
    };

    class V8ScriptEngine : public ScriptEngine {
    public:
        V8ScriptEngine();
        virtual ~V8ScriptEngine();
        virtual Scope* createScope() { return new V8Scope(this); }
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

        /**
         * Get the deadline monitor instance for the v8 ScriptEngine
         */
        DeadlineMonitor<V8Scope>* getDeadlineMonitor() { return &_deadlineMonitor; }

        typedef map<unsigned, V8Scope*> OpIdToScopeMap;
        mongo::mutex _globalInterruptLock;  // protects map of all operation ids -> scope
        OpIdToScopeMap _opToScopeMap;       // map of mongo op ids to scopes (protected by
                                            // _globalInterruptLock).
        DeadlineMonitor<V8Scope> _deadlineMonitor;
    };

    /**
     * Check for an error condition (e.g. empty handle, JS exception, OOM) after executing
     * a v8 operation.
     * @resultHandle         handle storing the result of the preceeding v8 operation
     * @try_catch            the active v8::TryCatch exception handler
     * @param reportError    if true, log an error message
     * @param assertOnError  if true, throw an exception if an error is detected
     *                       if false, return value indicates error state
     * @return true if an error was detected and assertOnError is set to false
     *         false if no error was detected
     */
    template <typename _HandleType>
    bool V8Scope::checkV8ErrorState(const _HandleType& resultHandle,
                                    const v8::TryCatch& try_catch,
                                    bool reportError,
                                    bool assertOnError) {
        bool haveError = false;

        if (try_catch.HasCaught() && try_catch.CanContinue()) {
            // normal JS exception
            _error = string("JavaScript execution failed: ") + v8ExceptionToSTLString(&try_catch);
            haveError = true;
        }
        else if (hasOutOfMemoryException()) {
            // out of memory exception (treated as terminal)
            _error = "JavaScript execution failed -- v8 is out of memory";
            haveError = true;
        }
        else if (resultHandle.IsEmpty() || try_catch.HasCaught()) {
            // terminal exception (due to empty handle, termination, etc.)
            _error = "JavaScript execution failed";
            haveError = true;
        }

        if (haveError) {
            if (reportError)
                log() << _error << endl;
            if (assertOnError)
                uasserted(16722, _error);
            return true;
        }

        return false;
    }

    extern ScriptEngine* globalScriptEngine;

}
