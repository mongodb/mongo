//engine_v8.h

/*    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/shared_ptr.hpp>
#include <v8.h>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/v8_deadline_monitor.h"
#include "mongo/scripting/v8-3.25_profiler.h"
#include "mongo/util/log.h"

/**
 * V8_SIMPLE_HEADER must be placed in any function called from a public API
 * that work with v8 handles (and/or must be within the V8Scope's isolate
 * and context).  Be sure to close the handle_scope if returning a v8::Handle!
 */
#define V8_SIMPLE_HEADER                                                                      \
        v8::Locker v8lock(_isolate);          /* acquire isolate lock */                      \
        v8::Isolate::Scope iscope(_isolate);  /* enter the isolate; exit when out of scope */ \
        v8::HandleScope handle_scope(_isolate); /* make the current scope own local */        \
                                                /* handles */                                 \
        v8::Context::Scope context_scope(getContext()); /* enter the context; exit when */    \
                                                        /* out of scope */

namespace mongo {

    class V8ScriptEngine;
    class V8Scope;
    class BSONHolder;

    typedef v8::Local<v8::Value> (*v8Function)(V8Scope* scope,
                                  const v8::FunctionCallbackInfo<v8::Value>& args);

    /**
     * The ObjTracker class keeps track of all weakly referenced v8 objects.  This is
     * required because v8 does not invoke the WeakReferenceCallback when shutting down
     * the context/isolate.  To track a new object, add an ObjTracker<MyObjType> member
     * variable to the V8Scope (if one does not already exist for that type).  Instead
     * of calling v8::Persistent::MakeWeak() directly, simply invoke track() with the
     * persistent handle and the pointer to be freed.
     */
    template <typename _ObjType>
    class ObjTracker {
    public:
        /** Track an object to be freed when it is no longer referenced in JavaScript.
         * Return handle to object instance shared pointer.
         * @param  instanceHandle  persistent handle to the weakly referenced object
         * @param  rawData         pointer to the object instance
         */
        v8::Local<v8::External> track(v8::Isolate* isolate, v8::Local<v8::Value> instanceHandle,
                                      _ObjType* instance) {
            TrackedPtr* collectionHandle = new TrackedPtr(isolate, instanceHandle, instance, this);
            _container.insert(collectionHandle);
            collectionHandle->_instanceHandle.SetWeak(collectionHandle, deleteOnCollect);
            return v8::External::New(isolate, &(collectionHandle->_objPtr));
        }
        /**
         * Free any remaining objects and their TrackedPtrs.  Invoked when the
         * V8Scope is destructed.
         */
        ~ObjTracker() {
            MONGO_LOG_DEFAULT_COMPONENT_LOCAL(::mongo::logger::LogComponent::kQuery);

            if (!_container.empty()) {
                LOG(1) << "freeing " << _container.size() << " uncollected "
                       << typeid(_ObjType).name() << " objects" << endl;
            }
            typename set<TrackedPtr*>::iterator it = _container.begin();
            while (it != _container.end()) {
                delete *it;
                _container.erase(it++);
            }
        }
    private:
        /**
         * Simple struct which contains a pointer to the tracked object, and a pointer
         * to the ObjTracker which owns it.  This is the argument supplied to v8's
         * WeakReferenceCallback and MakeWeak().
         */
        struct TrackedPtr {
        public:
            TrackedPtr(v8::Isolate* isolate, v8::Local<v8::Value>& instanceHandle,
                       _ObjType* instance, ObjTracker<_ObjType>* tracker) :
                _instanceHandle(isolate, instanceHandle),
                _objPtr(instance),
                _tracker(tracker) { }
            v8::Persistent<v8::Value> _instanceHandle;
            boost::shared_ptr<_ObjType> _objPtr;
            ObjTracker<_ObjType>* _tracker;
        };

        /**
         * v8 callback for weak persistent handles that have been marked for removal by the
         * garbage collector.  Signature conforms to v8's WeakReferenceCallback.
         * @param  data        Weak callback data. Contains pointer to the TrackedPtr instance.
         */
        static void deleteOnCollect(const v8::WeakCallbackData<v8::Value, TrackedPtr>& data) {
            boost::scoped_ptr<TrackedPtr> trackedPtr(data.GetParameter());
            invariant(trackedPtr.get());

            trackedPtr->_tracker->_container.erase(trackedPtr.get());
            trackedPtr->_instanceHandle.Reset();
        }

        // container for all TrackedPtrs created by this ObjTracker instance
        set<TrackedPtr*> _container;
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
        v8::Local<v8::Value> get(const char* field);

        virtual double getNumber(const char* field);
        virtual int getNumberInt(const char* field);
        virtual long long getNumberLongLong(const char* field);
        virtual string getString(const char* field);
        virtual bool getBoolean(const char* field);
        virtual BSONObj getObject(const char* field);

        virtual void setNumber(const char* field, double val);
        virtual void setString(const char* field, const StringData& val);
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
        void injectNative(const char* field, NativeFunction func, v8::Local<v8::Object>& obj,
                          void* data = 0);

        // These functions inject a function (either an unwrapped function pointer or a pre-wrapped
        // FunctionTemplate) into the provided object. If no object is provided, the function will
        // be injected at global scope. These functions take care of setting the function and class
        // name on the returned FunctionTemplate.
        v8::Local<v8::FunctionTemplate> injectV8Function(const char* name, v8Function func);
        v8::Local<v8::FunctionTemplate> injectV8Function(const char* name,
                                                          v8Function func,
                                                          v8::Local<v8::Object> obj);
        v8::Local<v8::FunctionTemplate> injectV8Function(const char* name,
                                                          v8::Local<v8::FunctionTemplate> ft,
                                                          v8::Local<v8::Object> obj);

        // Injects a method into the provided prototype
        v8::Local<v8::FunctionTemplate> injectV8Method(const char* name,
                                                        v8Function func,
                                                        v8::Local<v8::ObjectTemplate>& proto);
        v8::Local<v8::FunctionTemplate> createV8Function(v8Function func);
        virtual ScriptingFunction _createFunction(const char* code,
                                                  ScriptingFunction functionNumber = 0);
        v8::Local<v8::Function> __createFunction(const char* code,
                                                 ScriptingFunction functionNumber = 0);

        /**
         * Convert BSON types to v8 Javascript types
         */
        v8::Local<v8::Object> mongoToLZV8(const mongo::BSONObj& m, bool readOnly = false);
        v8::Local<v8::Value> mongoToV8Element(const BSONElement& f, bool readOnly = false);

        /**
         * Convert v8 Javascript types to BSON types
         */
        mongo::BSONObj v8ToMongo(v8::Local<v8::Object> obj, int depth = 0);
        void v8ToMongoElement(BSONObjBuilder& b,
                              const StringData& sname,
                              v8::Local<v8::Value> value,
                              int depth = 0,
                              BSONObj* originalParent = 0);
        void v8ToMongoObject(BSONObjBuilder& b,
                             const StringData& sname,
                             v8::Local<v8::Value> value,
                             int depth,
                             BSONObj* originalParent);
        void v8ToMongoNumber(BSONObjBuilder& b,
                             const StringData& elementName,
                             v8::Local<v8::Number> value,
                             BSONObj* originalParent);
        void v8ToMongoRegex(BSONObjBuilder& b,
                            const StringData& elementName,
                            v8::Local<v8::RegExp> v8Regex);
        void v8ToMongoDBRef(BSONObjBuilder& b,
                            const StringData& elementName,
                            v8::Local<v8::Object> obj);
        void v8ToMongoBinData(BSONObjBuilder& b,
                              const StringData& elementName,
                              v8::Local<v8::Object> obj);
        OID v8ToMongoObjectID(v8::Local<v8::Object> obj);

        v8::Local<v8::Value> newId(const OID& id);

        /**
         * Convert a JavaScript exception to a stl string.  Requires
         * access to the V8Scope instance to report source context information.
         */
        std::string v8ExceptionToSTLString(const v8::TryCatch* try_catch);

        /**
         * Create a V8 string with a local handle
         */
        inline v8::Local<v8::String> v8StringData(const StringData& str) {
            return v8::String::NewFromUtf8(_isolate, str.rawData(), v8::String::kNormalString,
                                           str.size());
        }

        /**
         * Get the isolate this scope belongs to (can be called from any thread, but v8 requires
         *  the new thread enter the isolate and context.  Only one thread can enter the isolate.
         */
        v8::Isolate* getIsolate() const { return _isolate; }

        /**
         * Get the JS context this scope executes within.
         */
        v8::Local<v8::Context> getContext() { return _context.Get(_isolate); }

        /**
         * Get the global JS object
         */
        v8::Local<v8::Object> getGlobal() { return _global.Get(_isolate); }

        ObjTracker<BSONHolder> bsonHolderTracker;
        ObjTracker<DBClientBase> dbClientBaseTracker;
        // Track both cursor and connection.
        // This ensures the connection outlives the cursor.
        struct DBConnectionAndCursor {
            boost::shared_ptr<DBClientBase> conn;
            boost::shared_ptr<DBClientCursor> cursor;
            DBConnectionAndCursor(boost::shared_ptr<DBClientBase> conn,
                                  boost::shared_ptr<DBClientCursor> cursor)
                : conn(conn), cursor(cursor) { }
        };
        ObjTracker<DBConnectionAndCursor> dbConnectionAndCursor;

        // These are all named after the JS constructor name + FT
        v8::Local<v8::FunctionTemplate> ObjectIdFT()       { return _ObjectIdFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> DBRefFT()          { return _DBRefFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> DBPointerFT()      { return _DBPointerFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> BinDataFT()        { return _BinDataFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> NumberLongFT()     { return _NumberLongFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> NumberIntFT()      { return _NumberIntFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> TimestampFT()      { return _TimestampFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> MinKeyFT()         { return _MinKeyFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> MaxKeyFT()         { return _MaxKeyFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> MongoFT()          { return _MongoFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> DBFT()             { return _DBFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> DBCollectionFT()   { return _DBCollectionFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> DBQueryFT()        { return _DBQueryFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> InternalCursorFT() {
            return _InternalCursorFT.Get(_isolate);
        }
        v8::Local<v8::FunctionTemplate> LazyBsonFT()       { return _LazyBsonFT.Get(_isolate); }
        v8::Local<v8::FunctionTemplate> ROBsonFT()         { return _ROBsonFT.Get(_isolate); }

        template <size_t N>
        v8::Local<v8::String> strLitToV8(const char (&str)[N]) {
            // Note that _strLitMap is keyed on string pointer not string
            // value. This is OK because each string literal has a constant
            // pointer for the program's lifetime. This works best if (but does
            // not require) the linker interns all string literals giving
            // identical strings used in different places the same pointer.

            StrLitMap::iterator it = _strLitMap.find(str);
            if (it != _strLitMap.end())
                return it->second.Get(_isolate);

            StringData sd (str, StringData::LiteralTag());
            v8::Local<v8::String> v8Str = v8StringData(sd);

            // Eternal should last as long as V8Scope exists.
            _strLitMap[str].Set(_isolate, v8Str);

            return v8Str;
        }

    private:
        /**
         * Recursion limit when converting from JS objects to BSON.
         */
        static const int objectDepthLimit = 150;

        /**
         * Attach data to obj such that the data has the same lifetime as the Object obj points to.
         * obj must have been created by either LazyBsonFT or ROBsonFT.
         */
        void wrapBSONObject(v8::Local<v8::Object> obj, BSONObj data, bool readOnly);

        /**
         * Trampoline to call a c++ function with a specific signature (V8Scope*,
         * v8::FunctionCallbackInfo<v8::Value>&).
         * Handles interruption, exceptions, etc.
         */
        static void v8Callback(const v8::FunctionCallbackInfo<v8::Value>& args);

        /**
         * Interpreter agnostic 'Native Callback' trampoline.  Note this is only called
         * from v8Callback().
         */
        static v8::Local<v8::Value> nativeCallback(V8Scope* scope,
            const v8::FunctionCallbackInfo<v8::Value>& args);

        /**
         * v8-specific implementations of basic global functions
         */
        static v8::Local<v8::Value> load(V8Scope* scope,
                                         const v8::FunctionCallbackInfo<v8::Value>& args);
        static v8::Local<v8::Value> Print(V8Scope* scope,
                                          const v8::FunctionCallbackInfo<v8::Value>& args);
        static v8::Local<v8::Value> Version(V8Scope* scope,
                                            const v8::FunctionCallbackInfo<v8::Value>& args);
        static v8::Local<v8::Value> GCV8(V8Scope* scope,
                                         const v8::FunctionCallbackInfo<v8::Value>& args);

        static v8::Local<v8::Value> startCpuProfiler(V8Scope* scope,
            const v8::FunctionCallbackInfo<v8::Value>& args);
        static v8::Local<v8::Value> stopCpuProfiler(V8Scope* scope,
            const v8::FunctionCallbackInfo<v8::Value>& args);
        static v8::Local<v8::Value> getCpuProfile(V8Scope* scope,
                                                  const v8::FunctionCallbackInfo<v8::Value>& args);

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
         * Create a new function; primarily used for BSON/V8 conversion.
         */
        v8::Local<v8::Value> newFunction(const StringData& code);

        template <typename _HandleType>
        bool checkV8ErrorState(const _HandleType& resultHandle,
                               const v8::TryCatch& try_catch,
                               bool reportError = true,
                               bool assertOnError = true);

        V8ScriptEngine* _engine;

        v8::Eternal<v8::Context> _context;
        v8::Eternal<v8::Object> _global;
        string _error;
        std::vector<v8::Eternal<v8::Value> > _funcs;

        enum ConnectState { NOT, LOCAL, EXTERNAL };
        ConnectState _connectState;

        // These are all named after the JS constructor name + FT
        v8::Eternal<v8::FunctionTemplate> _ObjectIdFT;
        v8::Eternal<v8::FunctionTemplate> _DBRefFT;
        v8::Eternal<v8::FunctionTemplate> _DBPointerFT;
        v8::Eternal<v8::FunctionTemplate> _BinDataFT;
        v8::Eternal<v8::FunctionTemplate> _NumberLongFT;
        v8::Eternal<v8::FunctionTemplate> _NumberIntFT;
        v8::Eternal<v8::FunctionTemplate> _TimestampFT;
        v8::Eternal<v8::FunctionTemplate> _MinKeyFT;
        v8::Eternal<v8::FunctionTemplate> _MaxKeyFT;
        v8::Eternal<v8::FunctionTemplate> _MongoFT;
        v8::Eternal<v8::FunctionTemplate> _DBFT;
        v8::Eternal<v8::FunctionTemplate> _DBCollectionFT;
        v8::Eternal<v8::FunctionTemplate> _DBQueryFT;
        v8::Eternal<v8::FunctionTemplate> _InternalCursorFT;
        v8::Eternal<v8::FunctionTemplate> _LazyBsonFT;
        v8::Eternal<v8::FunctionTemplate> _ROBsonFT;

        v8::Eternal<v8::Function> _jsRegExpConstructor;

        /// Like v8::Isolate* but calls Dispose() in destructor.
        class IsolateHolder {
            MONGO_DISALLOW_COPYING(IsolateHolder);
        public:
            IsolateHolder() :_isolate(NULL) {}
            ~IsolateHolder() {
                if (_isolate) {
                    _isolate->Dispose();
                    _isolate = NULL;
                }
            }

            void set(v8::Isolate* isolate) {
                fassert(17184, !_isolate);
                _isolate = isolate;
            }

            v8::Isolate* operator -> () const { return _isolate; };
            operator v8::Isolate* () const { return _isolate; };
        private:
            v8::Isolate* _isolate;
        };

        IsolateHolder _isolate; // NOTE: this must be destructed before the ObjTrackers

        V8CpuProfiler _cpuProfiler;

        // See comments in strLitToV8
        typedef unordered_map<const char*, v8::Eternal<v8::String> > StrLitMap;
        StrLitMap _strLitMap;

        mongo::mutex _interruptLock; // protects interruption-related flags
        bool _inNativeExecution;     // protected by _interruptLock
        bool _pendingKill;           // protected by _interruptLock
        int _opId;                   // op id for this scope
    };

    /// Helper to extract V8Scope for an Isolate
    inline V8Scope* getScope(v8::Isolate* isolate) {
        invariant(isolate);
        invariant(isolate->GetNumberOfDataSlots() >= 1U);
        uint32_t slot = 0;
        return static_cast<V8Scope*>(isolate->GetData(slot));
    }

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

    class BSONHolder {
    MONGO_DISALLOW_COPYING(BSONHolder);
    public:
        BSONHolder(V8Scope* scope, BSONObj obj, bool readOnly) :
            _scope(scope),
            _obj(obj.getOwned()),
            _modified(false),
            _readOnly(readOnly) {
            invariant(scope);
            if (_scope->getIsolate()) {
                // give hint v8's GC
                _scope->getIsolate()->AdjustAmountOfExternalAllocatedMemory(_obj.objsize());
            }
        }
        ~BSONHolder() {
            if (_scope->getIsolate()) {
                // if v8 is still up, send hint to GC
                _scope->getIsolate()->AdjustAmountOfExternalAllocatedMemory(-_obj.objsize());
            }
        }
        const V8Scope* _scope;
        const BSONObj _obj;
        bool _modified;
        const bool _readOnly;
        set<string> _removed;
    };

    /**
     * Check for an error condition (e.g. empty handle, JS exception, OOM) after executing
     * a v8 operation.
     * @resultHandle         handle storing the result of the preceding v8 operation
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
            _error = v8ExceptionToSTLString(&try_catch);
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
