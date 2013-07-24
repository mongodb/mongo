//engine_v8.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
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
 */

#include "mongo/scripting/engine_v8.h"

#include "mongo/base/init.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/scripting/v8_db.h"
#include "mongo/scripting/v8_utils.h"
#include "mongo/util/base64.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongoutils;

namespace mongo {

    // Generated symbols for JS files
    namespace JSFiles {
        extern const JSFile types;
        extern const JSFile assert;
    }

    // The  unwrapXXX functions extract internal fields from an object wrapped by wrapBSONObject.
    // These functions are currently only used in places that should always have the correct
    // type of object, however it may be possible for users to come up with a way to make these
    // called with the wrong type so calling code should always check the returns.
    static BSONHolder* unwrapHolder(V8Scope* scope, const v8::Handle<v8::Object>& obj) {
        // Warning: can't throw exceptions in this context.
        if (!scope->LazyBsonFT()->HasInstance(obj))
            return NULL;

        v8::Handle<v8::External> field = v8::Handle<v8::External>::Cast(obj->GetInternalField(0));
        if (field.IsEmpty() || !field->IsExternal())
            return 0;
        void* ptr = field->Value();
        return (BSONHolder*)ptr;
    }

    static BSONObj unwrapBSONObj(V8Scope* scope, const v8::Handle<v8::Object>& obj) {
        // Warning: can't throw exceptions in this context.
        BSONHolder* holder = unwrapHolder(scope, obj);
        return holder ? holder->_obj : BSONObj();
    }

    static v8::Handle<v8::Object> unwrapObject(V8Scope* scope, const v8::Handle<v8::Object>& obj) {
        // Warning: can't throw exceptions in this context.
        if (!scope->LazyBsonFT()->HasInstance(obj))
            return v8::Handle<v8::Object>();

        return obj->GetInternalField(1).As<v8::Object>();
    }

    void V8Scope::wrapBSONObject(v8::Handle<v8::Object> obj, BSONObj data, bool readOnly) {
        verify(LazyBsonFT()->HasInstance(obj));

        // Nothing below throws
        BSONHolder* holder = new BSONHolder(data);
        holder->_readOnly = readOnly;
        holder->_scope = this;
        obj->SetInternalField(0, v8::External::New(holder)); // Holder
        obj->SetInternalField(1, v8::Object::New()); // Object
        v8::Persistent<v8::Object> p = v8::Persistent<v8::Object>::New(obj);
        bsonHolderTracker.track(p, holder);
    }

    static v8::Handle<v8::Value> namedGet(v8::Local<v8::String> name,
                                          const v8::AccessorInfo& info) {
        v8::HandleScope handle_scope;
        v8::Handle<v8::Value> val;
        try {
            V8Scope* scope = getScope(info.GetIsolate());
            v8::Handle<v8::Object> realObject = unwrapObject(scope, info.Holder());
            if (realObject.IsEmpty()) return v8::Handle<v8::Value>();
            if (realObject->HasOwnProperty(name)) {
                // value already cached or added
                return handle_scope.Close(realObject->Get(name));
            }

            string key = toSTLString(name);
            BSONHolder* holder = unwrapHolder(scope, info.Holder());
            if (!holder || holder->_removed.count(key))
                return handle_scope.Close(v8::Handle<v8::Value>());

            BSONObj obj = holder->_obj;
            BSONElement elmt = obj.getField(key.c_str());
            if (elmt.eoo())
                return handle_scope.Close(v8::Handle<v8::Value>());

            val = scope->mongoToV8Element(elmt, holder->_readOnly);

            if (obj.objsize() > 128 || val->IsObject()) {
                // Only cache if expected to help (large BSON) or is required due to js semantics
                realObject->Set(name, val);
            }

            if (elmt.type() == mongo::Object || elmt.type() == mongo::Array) {
              // if accessing a subobject, it may get modified and base obj would not know
              // have to set base as modified, which means some optim is lost
              holder->_modified = true;
            }
        }
        catch (const DBException &dbEx) {
            return v8AssertionException(dbEx.toString());
        }
        catch (...) {
            return v8AssertionException(string("error getting property ") + toSTLString(name));
        }
        return handle_scope.Close(val);
    }

    static v8::Handle<v8::Value> namedGetRO(v8::Local<v8::String> name,
                                            const v8::AccessorInfo &info) {
        return namedGet(name, info);
    }

    static v8::Handle<v8::Value> namedSet(v8::Local<v8::String> name,
                                          v8::Local<v8::Value> value_obj,
                                          const v8::AccessorInfo& info) {
        string key = toSTLString(name);
        V8Scope* scope = getScope(info.GetIsolate());
        BSONHolder* holder = unwrapHolder(scope, info.Holder());
        if (!holder) return v8::Handle<v8::Value>();
        holder->_removed.erase(key);
        holder->_modified = true;

        v8::Handle<v8::Object> realObject = unwrapObject(scope, info.Holder());
        if (realObject.IsEmpty()) return v8::Handle<v8::Value>();
        realObject->Set(name, value_obj);
        return value_obj;
    }

    static v8::Handle<v8::Array> namedEnumerator(const v8::AccessorInfo &info) {
        v8::HandleScope handle_scope;
        V8Scope* scope = getScope(info.GetIsolate());
        BSONHolder* holder = unwrapHolder(scope, info.Holder());
        if (!holder) return v8::Handle<v8::Array>();
        BSONObj obj = holder->_obj;
        v8::Handle<v8::Array> out = v8::Array::New();
        int outIndex = 0;

        unordered_set<StringData, StringData::Hasher> added;
        // note here that if keys are parseable number, v8 will access them using index
        for (BSONObjIterator it(obj); it.more();) {
            const BSONElement& f = it.next();
            StringData sname (f.fieldName(), f.fieldNameSize()-1);
            if (holder->_removed.count(sname.toString()))
                continue;

            v8::Handle<v8::String> name = scope->v8StringData(sname);
            added.insert(sname);
            out->Set(outIndex++, name);
        }


        v8::Handle<v8::Object> realObject = unwrapObject(scope, info.Holder());
        if (realObject.IsEmpty()) return v8::Handle<v8::Array>();
        v8::Handle<v8::Array> fields = realObject->GetOwnPropertyNames();
        const int len = fields->Length();
        for (int field=0; field < len; field++) {
            v8::Handle<v8::String> name = fields->Get(field).As<v8::String>();
            V8String sname (name);
            if (added.count(sname))
                continue;
            out->Set(outIndex++, name);
        }
        return handle_scope.Close(out);
    }

    v8::Handle<v8::Boolean> namedDelete(v8::Local<v8::String> name, const v8::AccessorInfo& info) {
        v8::HandleScope handle_scope;
        string key = toSTLString(name);
        V8Scope* scope = getScope(info.GetIsolate());
        BSONHolder* holder = unwrapHolder(scope, info.Holder());
        if (!holder) return v8::Handle<v8::Boolean>();
        holder->_removed.insert(key);
        holder->_modified = true;

        v8::Handle<v8::Object> realObject = unwrapObject(scope, info.Holder());
        if (realObject.IsEmpty()) return v8::Handle<v8::Boolean>();
        realObject->Delete(name);
        return v8::True();
    }

    static v8::Handle<v8::Value> indexedGet(uint32_t index, const v8::AccessorInfo &info) {
        v8::HandleScope handle_scope;
        v8::Handle<v8::Value> val;
        try {
            V8Scope* scope = getScope(info.GetIsolate());
            v8::Handle<v8::Object> realObject = unwrapObject(scope, info.Holder());
            if (realObject.IsEmpty()) return v8::Handle<v8::Value>();
            if (realObject->Has(index)) {
                // value already cached or added
                return handle_scope.Close(realObject->Get(index));
            }
            string key = str::stream() << index;

            BSONHolder* holder = unwrapHolder(scope, info.Holder());
            if (!holder) return v8::Handle<v8::Value>();
            if (holder->_removed.count(key))
                return handle_scope.Close(v8::Handle<v8::Value>());

            BSONObj obj = holder->_obj;
            BSONElement elmt = obj.getField(key);
            if (elmt.eoo())
                return handle_scope.Close(v8::Handle<v8::Value>());
            val = scope->mongoToV8Element(elmt, holder->_readOnly);
            realObject->Set(index, val);

            if (elmt.type() == mongo::Object || elmt.type() == mongo::Array) {
                // if accessing a subobject, it may get modified and base obj would not know
                // have to set base as modified, which means some optim is lost
                holder->_modified = true;
            }
        }
        catch (const DBException &dbEx) {
            return v8AssertionException(dbEx.toString());
        }
        catch (...) {
            return v8AssertionException(str::stream() << "error getting indexed property "
                                                      << index);
        }
        return handle_scope.Close(val);
    }

    v8::Handle<v8::Boolean> indexedDelete(uint32_t index, const v8::AccessorInfo& info) {
        string key = str::stream() << index;
        V8Scope* scope = getScope(info.GetIsolate());
        BSONHolder* holder = unwrapHolder(scope, info.Holder());
        if (!holder) return v8::Handle<v8::Boolean>();
        holder->_removed.insert(key);
        holder->_modified = true;

        // also delete in JS obj
        v8::Handle<v8::Object> realObject = unwrapObject(scope, info.Holder());
        if (realObject.IsEmpty()) return v8::Handle<v8::Boolean>();
        realObject->Delete(index);
        return v8::True();
    }

    static v8::Handle<v8::Value> indexedGetRO(uint32_t index, const v8::AccessorInfo &info) {
        return indexedGet(index, info);
    }

    static v8::Handle<v8::Value> indexedSet(uint32_t index, v8::Local<v8::Value> value_obj,
                                            const v8::AccessorInfo& info) {
        string key = str::stream() << index;
        V8Scope* scope = getScope(info.GetIsolate());
        BSONHolder* holder = unwrapHolder(scope, info.Holder());
        if (!holder) return v8::Handle<v8::Value>();
        holder->_removed.erase(key);
        holder->_modified = true;

        v8::Handle<v8::Object> realObject = unwrapObject(scope, info.Holder());
        if (realObject.IsEmpty()) return v8::Handle<v8::Value>();
        realObject->Set(index, value_obj);
        return value_obj;
    }

    v8::Handle<v8::Value> NamedReadOnlySet(v8::Local<v8::String> property,
                                           v8::Local<v8::Value> value,
                                           const v8::AccessorInfo& info) {
        cout << "cannot write property " << V8String(property) << " to read-only object" << endl;
        return value;
    }

    v8::Handle<v8::Boolean> NamedReadOnlyDelete(v8::Local<v8::String> property,
                                                const v8::AccessorInfo& info) {
        cout << "cannot delete property " << V8String(property) << " from read-only object" << endl;
        return v8::Boolean::New(false);
    }

    v8::Handle<v8::Value> IndexedReadOnlySet(uint32_t index, v8::Local<v8::Value> value,
                                             const v8::AccessorInfo& info) {
        cout << "cannot write property " << index << " to read-only array" << endl;
        return value;
    }

    v8::Handle<v8::Boolean> IndexedReadOnlyDelete(uint32_t index, const v8::AccessorInfo& info) {
        cout << "cannot delete property " << index << " from read-only array" << endl;
        return v8::Boolean::New(false);
    }

    /**
     * GC Prologue and Epilogue constants (used to display description constants)
     */
    struct GCPrologueState { static const char* name; };
    const char* GCPrologueState::name = "prologue";
    struct GCEpilogueState { static const char* name; };
    const char* GCEpilogueState::name = "epilogue";

    template <typename _GCState>
    void gcCallback(v8::GCType type, v8::GCCallbackFlags flags) {
        if (!logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1)))
             // don't collect stats unless verbose
             return;

         v8::HeapStatistics stats;
         v8::V8::GetHeapStatistics(&stats);
         log() << "V8 GC " << _GCState::name
               << " heap stats - "
               << " total: " << stats.total_heap_size()
               << " exec: " << stats.total_heap_size_executable()
               << " used: " << stats.used_heap_size()<< " limit: "
               << stats.heap_size_limit()
               << endl;
     }

     V8ScriptEngine::V8ScriptEngine() :
         _globalInterruptLock("GlobalV8InterruptLock"),
         _opToScopeMap(),
         _deadlineMonitor() {
     }

     V8ScriptEngine::~V8ScriptEngine() {
     }

     void ScriptEngine::setup() {
         if (!globalScriptEngine) {
             globalScriptEngine = new V8ScriptEngine();
         }
     }

     std::string ScriptEngine::getInterpreterVersionString() {
         return "V8 3.12.19";
     }

     void V8ScriptEngine::interrupt(unsigned opId) {
         mongo::mutex::scoped_lock intLock(_globalInterruptLock);
         OpIdToScopeMap::iterator iScope = _opToScopeMap.find(opId);
         if (iScope == _opToScopeMap.end()) {
             // got interrupt request for a scope that no longer exists
             LOG(1) << "received interrupt request for unknown op: " << opId
                    << printKnownOps_inlock() << endl;
             return;
         }
         LOG(1) << "interrupting op: " << opId << printKnownOps_inlock() << endl;
         iScope->second->kill();
     }

     void V8ScriptEngine::interruptAll() {
         mongo::mutex::scoped_lock interruptLock(_globalInterruptLock);
         for (OpIdToScopeMap::iterator iScope = _opToScopeMap.begin();
              iScope != _opToScopeMap.end(); ++iScope) {
             iScope->second->kill();
         }
     }

     void V8Scope::registerOpId() {
         scoped_lock giLock(_engine->_globalInterruptLock);
         if (_engine->haveGetCurrentOpIdCallback()) {
             // this scope has an associated operation
             _opId = _engine->getCurrentOpId();
             _engine->_opToScopeMap[_opId] = this;
         }
         else
             // no associated op id (e.g. running from shell)
             _opId = 0;
         LOG(2) << "V8Scope " << static_cast<const void*>(this) << " registered for op " << _opId << endl;
     }

     void V8Scope::unregisterOpId() {
         scoped_lock giLock(_engine->_globalInterruptLock);
         LOG(2) << "V8Scope " << static_cast<const void*>(this) << " unregistered for op " << _opId << endl;
        if (_engine->haveGetCurrentOpIdCallback() || _opId != 0) {
            // scope is currently associated with an operation id
            V8ScriptEngine::OpIdToScopeMap::iterator it = _engine->_opToScopeMap.find(_opId);
            if (it != _engine->_opToScopeMap.end())
                _engine->_opToScopeMap.erase(it);
        }
    }

    bool V8Scope::nativePrologue() {
        v8::Locker l(_isolate);
        mongo::mutex::scoped_lock cbEnterLock(_interruptLock);
        if (v8::V8::IsExecutionTerminating(_isolate)) {
            LOG(2) << "v8 execution interrupted.  isolate: " << static_cast<const void*>(_isolate) << endl;
            return false;
        }
        if (_pendingKill || globalScriptEngine->interrupted()) {
            // kill flag was set before entering our callback
            LOG(2) << "marked for death while leaving callback.  isolate: " << static_cast<const void*>(_isolate) << endl;
            v8::V8::TerminateExecution(_isolate);
            return false;
        }
        _inNativeExecution = true;
        return true;
    }

    bool V8Scope::nativeEpilogue() {
        v8::Locker l(_isolate);
        mongo::mutex::scoped_lock cbLeaveLock(_interruptLock);
        _inNativeExecution = false;
        if (v8::V8::IsExecutionTerminating(_isolate)) {
            LOG(2) << "v8 execution interrupted.  isolate: " << static_cast<const void*>(_isolate) << endl;
            return false;
        }
        if (_pendingKill || globalScriptEngine->interrupted()) {
            LOG(2) << "marked for death while leaving callback.  isolate: " << static_cast<const void*>(_isolate) << endl;
            v8::V8::TerminateExecution(_isolate);
            return false;
        }
        return true;
    }

    void V8Scope::kill() {
        mongo::mutex::scoped_lock interruptLock(_interruptLock);
        if (!_inNativeExecution) {
            // Set the TERMINATE flag on the stack guard for this isolate.
            // This won't happen between calls to nativePrologue and nativeEpilogue().
            v8::V8::TerminateExecution(_isolate);
            LOG(1) << "killing v8 scope.  isolate: " << static_cast<const void*>(_isolate) << endl;
        }
        LOG(1) << "marking v8 scope for death.  isolate: " << static_cast<const void*>(_isolate) << endl;
        _pendingKill = true;
    }

    /** check if there is a pending killOp request */
    bool V8Scope::isKillPending() const {
        return _pendingKill || _engine->interrupted();
    }

    /**
     * Display a list of all known ops (for verbose output)
     */
    std::string V8ScriptEngine::printKnownOps_inlock() {
        stringstream out;
        if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(2))) {
            out << "  known ops: " << endl;
            for(OpIdToScopeMap::iterator iSc = _opToScopeMap.begin();
                iSc != _opToScopeMap.end(); ++iSc) {
                out << "  " << iSc->first << endl;
            }
        }
        return out.str();
    }


    V8Scope::V8Scope(V8ScriptEngine * engine)
        : _engine(engine),
          _connectState(NOT),
          _cpuProfiler(),
          _interruptLock("ScopeInterruptLock"),
          _inNativeExecution(true),
          _pendingKill(false) {

        // create new isolate and enter it via a scope
        _isolate = v8::Isolate::New();
        v8::Isolate::Scope iscope(_isolate);

        // lock the isolate and enter the context
        v8::Locker l(_isolate);
        v8::HandleScope handleScope;
        _context = v8::Context::New();
        v8::Context::Scope context_scope(_context);

        _isolate->SetData(this);

        // display heap statistics on MarkAndSweep GC run
        v8::V8::AddGCPrologueCallback(gcCallback<GCPrologueState>, v8::kGCTypeMarkSweepCompact);
        v8::V8::AddGCEpilogueCallback(gcCallback<GCEpilogueState>, v8::kGCTypeMarkSweepCompact);

        // if the isolate runs out of heap space, raise a flag on the StackGuard instead of
        // calling abort()
        v8::V8::IgnoreOutOfMemoryException();

        // create a global (rooted) object
        _global = v8::Persistent<v8::Object>::New(_context->Global());

        // Grab the RegExp constructor before user code gets a chance to change it. This ensures
        // we can always construct proper RegExps from C++.
        v8::Handle<v8::Value> regexp = _global->Get(strLitToV8("RegExp"));
        verify(regexp->IsFunction());
        _jsRegExpConstructor = v8::Persistent<v8::Function>::New(regexp.As<v8::Function>());

        // initialize lazy object template
        _LazyBsonFT = v8::Persistent<v8::FunctionTemplate>::New(v8::FunctionTemplate::New());
        LazyBsonFT()->InstanceTemplate()->SetInternalFieldCount(2);
        LazyBsonFT()->InstanceTemplate()->SetNamedPropertyHandler(
                namedGet, namedSet, NULL, namedDelete, namedEnumerator);
        LazyBsonFT()->InstanceTemplate()->SetIndexedPropertyHandler(
                indexedGet, indexedSet, NULL, indexedDelete, namedEnumerator);
        LazyBsonFT()->PrototypeTemplate()->Set(strLitToV8("_bson"),
                                               v8::Boolean::New(true),
                                               v8::DontEnum);

        _ROBsonFT = v8::Persistent<v8::FunctionTemplate>::New(v8::FunctionTemplate::New());
        ROBsonFT()->Inherit(LazyBsonFT()); // This makes LazyBsonFT()->HasInstance() true
        ROBsonFT()->InstanceTemplate()->SetInternalFieldCount(2);
        ROBsonFT()->InstanceTemplate()->SetNamedPropertyHandler(
                namedGetRO, NamedReadOnlySet, NULL, NamedReadOnlyDelete, namedEnumerator);
        ROBsonFT()->InstanceTemplate()->SetIndexedPropertyHandler(
                indexedGetRO, IndexedReadOnlySet, NULL, IndexedReadOnlyDelete, NULL);
        ROBsonFT()->PrototypeTemplate()->Set(strLitToV8("_bson"),
                                             v8::Boolean::New(true),
                                             v8::DontEnum);

        injectV8Function("print", Print);
        injectV8Function("version", Version);  // TODO: remove
        injectV8Function("gc", GCV8);
        // injectV8Function("startCpuProfiler", startCpuProfiler);
        // injectV8Function("stopCpuProfiler", stopCpuProfiler);
        // injectV8Function("getCpuProfile", getCpuProfile);

        // install BSON functions in the global object
        installBSONTypes();

        // load JS helpers (dependancy: installBSONTypes)
        execSetup(JSFiles::assert);
        execSetup(JSFiles::types);

        // install process-specific utilities in the global scope (dependancy: types.js, assert.js)
        if (_engine->_scopeInitCallback)
            _engine->_scopeInitCallback(*this);

        // install global utility functions
        installGlobalUtils(*this);

        registerOpId();
    }

    V8Scope::~V8Scope() {
        unregisterOpId();
        {
            V8_SIMPLE_HEADER
            for(unsigned i = 0; i < _funcs.size(); ++i)
                _funcs[ i ].Dispose();
            _funcs.clear();
            _global.Dispose();
            _context.Dispose();
            // Note: This block is unnecessary since we destroy the v8 Heap (Isolate) immediately
            //       after. Leaving in for now, but nothing new should be added.
        }
        _isolate->Dispose();
        // set the isolate to NULL so ObjTracker destructors know that v8 is no longer reachable
        _isolate = NULL;
    }

    bool V8Scope::hasOutOfMemoryException() {
        V8_SIMPLE_HEADER
        if (!_context.IsEmpty())
            return _context->HasOutOfMemoryException();
        return false;
    }

    v8::Handle<v8::Value> V8Scope::load(V8Scope* scope, const v8::Arguments &args) {
        v8::Context::Scope context_scope(scope->_context);
        for (int i = 0; i < args.Length(); ++i) {
            std::string filename(toSTLString(args[i]));
            if (!scope->execFile(filename, false, true)) {
                return v8AssertionException(string("error loading js file: ") + filename);
            }
        }
        return v8::True();
    }

    v8::Handle<v8::Value> V8Scope::nativeCallback(V8Scope* scope, const v8::Arguments &args) {
        BSONObj ret;
        string exceptionText;
        v8::HandleScope handle_scope;
        try {
            v8::Local<v8::External> f =
                    v8::External::Cast(*args.Callee()->Get(scope->strLitToV8("_native_function")));
            NativeFunction function = (NativeFunction)(f->Value());
            v8::Local<v8::External> data =
                    v8::External::Cast(*args.Callee()->Get(scope->strLitToV8("_native_data")));
            BSONObjBuilder b;
            for (int i = 0; i < args.Length(); ++i)
                scope->v8ToMongoElement(b, BSONObjBuilder::numStr(i), args[i]);
            BSONObj nativeArgs = b.obj();
            ret = function(nativeArgs, data->Value());
        }
        catch (const std::exception &e) {
            exceptionText = e.what();
        }
        catch (...) {
            exceptionText = "unknown exception in V8Scope::nativeCallback";
        }
        if (!exceptionText.empty()) {
            return v8AssertionException(exceptionText);
        }
        return handle_scope.Close(scope->mongoToV8Element(ret.firstElement()));
    }

    v8::Handle<v8::Value> V8Scope::v8Callback(const v8::Arguments &args) {
        v8::HandleScope handle_scope;
        V8Scope* scope = getScope(args.GetIsolate());

        if (!scope->nativePrologue())
            // execution terminated
            return v8::Undefined();

        v8::Local<v8::External> f =
                v8::External::Cast(*args.Callee()->Get(scope->strLitToV8("_v8_function")));
        v8Function function = (v8Function)(f->Value());
        v8::Handle<v8::Value> ret;
        string exceptionText;

        try {
            // execute the native function
            ret = function(scope, args);
        }
        catch (const std::exception& e) {
            exceptionText = e.what();
        }
        catch (...) {
            exceptionText = "unknown exception in V8Scope::v8Callback";
        }

        if (!scope->nativeEpilogue())
            // execution terminated
            return v8::Undefined();

        if (!exceptionText.empty()) {
            return v8AssertionException(exceptionText);
        }
        return handle_scope.Close(ret);
    }

    void V8Scope::init(const BSONObj * data) {
        if (! data)
            return;

        BSONObjIterator i(*data);
        while (i.more()) {
            BSONElement e = i.next();
            setElement(e.fieldName(), e);
        }
    }

    void V8Scope::setNumber(const char * field, double val) {
        V8_SIMPLE_HEADER
        _global->ForceSet(v8StringData(field), v8::Number::New(val));
    }

    void V8Scope::setString(const char * field, const StringData& val) {
        V8_SIMPLE_HEADER
        _global->ForceSet(v8StringData(field), v8::String::New(val.rawData(), val.size()));
    }

    void V8Scope::setBoolean(const char * field, bool val) {
        V8_SIMPLE_HEADER
        _global->ForceSet(v8StringData(field), v8::Boolean::New(val));
    }

    void V8Scope::setElement(const char *field, const BSONElement& e) {
        V8_SIMPLE_HEADER
        _global->ForceSet(v8StringData(field), mongoToV8Element(e));
    }

    void V8Scope::setObject(const char *field, const BSONObj& obj, bool readOnly) {
        V8_SIMPLE_HEADER
        _global->ForceSet(v8StringData(field),
                          mongoToLZV8(obj, readOnly ? v8::ReadOnly : v8::None));
    }

    int V8Scope::type(const char *field) {
        V8_SIMPLE_HEADER
        v8::Handle<v8::Value> v = get(field);
        if (v->IsNull())
            return jstNULL;
        if (v->IsUndefined())
            return Undefined;
        if (v->IsString())
            return String;
        if (v->IsFunction())
            return Code;
        if (v->IsArray())
            return Array;
        if (v->IsBoolean())
            return Bool;
        // needs to be explicit NumberInt to use integer
//        if (v->IsInt32())
//            return NumberInt;
        if (v->IsNumber())
            return NumberDouble;
        if (v->IsExternal()) {
            uassert(10230,  "can't handle external yet", 0);
            return -1;
        }
        if (v->IsDate())
            return Date;
        if (v->IsObject())
            return Object;

        uasserted(12509, str::stream() << "unable to get type of field " << field);
    }

    v8::Handle<v8::Value> V8Scope::get(const char * field) {
        return _global->Get(v8StringData(field));
    }

    double V8Scope::getNumber(const char *field) {
        V8_SIMPLE_HEADER
        return get(field)->ToNumber()->Value();
    }

    int V8Scope::getNumberInt(const char *field) {
        V8_SIMPLE_HEADER
        return get(field)->ToInt32()->Value();
    }

    long long V8Scope::getNumberLongLong(const char *field) {
        V8_SIMPLE_HEADER
        return get(field)->ToInteger()->Value();
    }

    string V8Scope::getString(const char *field) {
        V8_SIMPLE_HEADER
        return toSTLString(get(field));
    }

    bool V8Scope::getBoolean(const char *field) {
        V8_SIMPLE_HEADER
        return get(field)->ToBoolean()->Value();
    }

    BSONObj V8Scope::getObject(const char * field) {
        V8_SIMPLE_HEADER
        v8::Handle<v8::Value> v = get(field);
        if (v->IsNull() || v->IsUndefined())
            return BSONObj();
        uassert(10231,  "not an object", v->IsObject());
        return v8ToMongo(v->ToObject());
    }

    v8::Handle<v8::FunctionTemplate> getNumberLongFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> numberLong = scope->createV8Function(numberLongInit);
        v8::Handle<v8::ObjectTemplate> proto = numberLong->PrototypeTemplate();
        scope->injectV8Method("valueOf", numberLongValueOf, proto);
        scope->injectV8Method("toNumber", numberLongToNumber, proto);
        scope->injectV8Method("toString", numberLongToString, proto);
        return numberLong;
    }

    v8::Handle<v8::FunctionTemplate> getNumberIntFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> numberInt = scope->createV8Function(numberIntInit);
        v8::Handle<v8::ObjectTemplate> proto = numberInt->PrototypeTemplate();
        scope->injectV8Method("valueOf", numberIntValueOf, proto);
        scope->injectV8Method("toNumber", numberIntToNumber, proto);
        scope->injectV8Method("toString", numberIntToString, proto);
        return numberInt;
    }

    v8::Handle<v8::FunctionTemplate> getBinDataFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> binData = scope->createV8Function(binDataInit);
        binData->InstanceTemplate()->SetInternalFieldCount(1);
        v8::Handle<v8::ObjectTemplate> proto = binData->PrototypeTemplate();
        scope->injectV8Method("toString", binDataToString, proto);
        scope->injectV8Method("base64", binDataToBase64, proto);
        scope->injectV8Method("hex", binDataToHex, proto);
        return binData;
    }

    v8::Handle<v8::FunctionTemplate> getTimestampFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> ts = scope->createV8Function(dbTimestampInit);
        return ts;
    }

    v8::Handle<v8::Value> minKeyToJson(V8Scope* scope, const v8::Arguments& args) {
        // MinKey can't just be an object like {$minKey:1} since insert() checks for fields that
        // start with $ and raises an error. See DBCollection.prototype._validateForStorage().
        return scope->strLitToV8("{ \"$minKey\" : 1 }");
    }

    v8::Handle<v8::Value> minKeyCall(const v8::Arguments& args) {
        // The idea here is that MinKey and MaxKey are singleton callable objects
        // that return the singleton when called. This enables all instances to
        // compare == and === to MinKey even if created by "new MinKey()" in JS.
        V8Scope* scope = getScope(args.GetIsolate());

        v8::Handle<v8::Function> func = scope->MinKeyFT()->GetFunction();
        v8::Handle<v8::String> name = scope->strLitToV8("singleton");
        v8::Handle<v8::Value> singleton = func->GetHiddenValue(name);
        if (!singleton.IsEmpty())
            return singleton;

        if (!args.IsConstructCall())
            return func->NewInstance();

        verify(scope->MinKeyFT()->HasInstance(args.This()));

        func->SetHiddenValue(name, args.This());
        return v8::Undefined();
    }

    v8::Handle<v8::FunctionTemplate> getMinKeyFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> myTemplate = v8::FunctionTemplate::New(minKeyCall);
        myTemplate->InstanceTemplate()->SetCallAsFunctionHandler(minKeyCall);
        myTemplate->PrototypeTemplate()->Set(
                "tojson", scope->createV8Function(minKeyToJson)->GetFunction());
        myTemplate->SetClassName(scope->strLitToV8("MinKey"));
        return myTemplate;
    }

    v8::Handle<v8::Value> maxKeyToJson(V8Scope* scope, const v8::Arguments& args) {
        return scope->strLitToV8("{ \"$maxKey\" : 1 }");
    }

    v8::Handle<v8::Value> maxKeyCall(const v8::Arguments& args) {
        // See comment in minKeyCall.
        V8Scope* scope = getScope(args.GetIsolate());

        v8::Handle<v8::Function> func = scope->MaxKeyFT()->GetFunction();
        v8::Handle<v8::String> name = scope->strLitToV8("singleton");
        v8::Handle<v8::Value> singleton = func->GetHiddenValue(name);
        if (!singleton.IsEmpty())
            return singleton;

        if (!args.IsConstructCall())
            return func->NewInstance();

        verify(scope->MaxKeyFT()->HasInstance(args.This()));

        func->SetHiddenValue(name, args.This());
        return v8::Undefined();
    }

    v8::Handle<v8::FunctionTemplate> getMaxKeyFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> myTemplate = v8::FunctionTemplate::New(maxKeyCall);
        myTemplate->InstanceTemplate()->SetCallAsFunctionHandler(maxKeyCall);
        myTemplate->PrototypeTemplate()->Set(
                "tojson", scope->createV8Function(maxKeyToJson)->GetFunction());
        myTemplate->SetClassName(scope->strLitToV8("MaxKey"));
        return myTemplate;
    }

    std::string V8Scope::v8ExceptionToSTLString(const v8::TryCatch* try_catch) {
        stringstream ss;
        v8::String::Utf8Value exceptionText(try_catch->Exception());
        ss << *exceptionText;

        // get the exception message
        v8::Handle<v8::Message> message = try_catch->Message();
        if (message.IsEmpty())
            return ss.str();

        // get the resource (e.g. file or internal call)
        v8::String::Utf8Value resourceName(message->GetScriptResourceName());
        if (!*resourceName)
            return ss.str();

        string resourceNameString = *resourceName;
        if (resourceNameString.compare("undefined") == 0)
            return ss.str();
        if (resourceNameString.find("_funcs") == 0) {
            // script loaded from __createFunction
            string code;
            // find the source script based on the resource name supplied to v8::Script::Compile().
            // this is accomplished by converting the integer after the '_funcs' prefix.
            unsigned int funcNum = str::toUnsigned(resourceNameString.substr(6));
            for (map<string, ScriptingFunction>::iterator it = getFunctionCache().begin();
                 it != getFunctionCache().end();
                 ++it) {
                if (it->second == funcNum) {
                    code = it->first;
                    break;
                }
            }
            if (!code.empty()) {
                // append surrounding code (padded with up to 20 characters on each side)
                int startPos = message->GetStartPosition();
                const int kPadding = 20;
                if (startPos - kPadding < 0)
                    // lower bound exceeded
                    startPos = 0;
                else
                    startPos -= kPadding;

                int displayRange = message->GetEndPosition();
                if (displayRange + kPadding > static_cast<int>(code.length()))
                    // upper bound exceeded
                    displayRange -= startPos;
                else
                    // compensate for startPos padding
                    displayRange = (displayRange - startPos) + kPadding;

                if (startPos > static_cast<int>(code.length()) ||
                    displayRange > static_cast<int>(code.length()))
                    return ss.str();

                string codeNear = code.substr(startPos, displayRange);
                for (size_t newLine = codeNear.find('\n');
                     newLine != string::npos;
                     newLine = codeNear.find('\n')) {
                    if (static_cast<int>(newLine) > displayRange - kPadding) {
                        // truncate at first newline past the reported end position
                        codeNear = codeNear.substr(0, newLine - 1);
                        break;
                    }
                    // convert newlines to spaces
                    codeNear.replace(newLine, 1, " ");
                }
                // trim leading chars
                codeNear = str::ltrim(codeNear);
                ss << " near '" << codeNear << "' ";
                const int linenum = message->GetLineNumber();
                if (linenum != 1)
                    ss << " (line " << linenum << ")";
            }
        }
        else if (resourceNameString.find("(shell") == 0) {
            // script loaded from shell input -- simply print the error
        }
        else {
            // script loaded from file
            ss << " at " << *resourceName;
            const int linenum = message->GetLineNumber();
            if (linenum != 1) ss << ":" << linenum;
        }
        return ss.str();
    }

    // --- functions -----

    bool hasFunctionIdentifier(const StringData& code) {
        if (code.size() < 9 || code.find("function") != 0 )
            return false;

        return code[8] == ' ' || code[8] == '(';
    }

    v8::Local<v8::Function> V8Scope::__createFunction(const char* raw,
                                                      ScriptingFunction functionNumber) {
        v8::HandleScope handle_scope;
        v8::TryCatch try_catch;
        raw = jsSkipWhiteSpace(raw);
        string code = raw;
        if (!hasFunctionIdentifier(code)) {
            if (code.find('\n') == string::npos &&
                    ! hasJSReturn(code) &&
                    (code.find(';') == string::npos || code.find(';') == code.size() - 1)) {
                code = "return " + code;
            }
            code = "function(){ " + code + "}";
        }

        string fn = str::stream() << "_funcs" << functionNumber;
        code = str::stream() << fn << " = " << code;

        v8::Handle<v8::Script> script = v8::Script::Compile(
                                            v8::String::New(code.c_str(), code.length()),
                                            v8::String::New(fn.c_str()));

        // throw on error
        checkV8ErrorState(script, try_catch);

        v8::Local<v8::Value> result = script->Run();

        // throw on error
        checkV8ErrorState(result, try_catch);

        return handle_scope.Close(v8::Handle<v8::Function>(
                v8::Function::Cast(*_global->Get(v8::String::New(fn.c_str())))));
    }

    ScriptingFunction V8Scope::_createFunction(const char* raw, ScriptingFunction functionNumber) {
        V8_SIMPLE_HEADER
        v8::Local<v8::Value> ret = __createFunction(raw, functionNumber);
        v8::Persistent<v8::Value> f = v8::Persistent<v8::Value>::New(ret);
        uassert(10232, "not a function", f->IsFunction());
        _funcs.push_back(f);
        return functionNumber;
    }

    void V8Scope::setFunction(const char* field, const char* code) {
        V8_SIMPLE_HEADER
        _global->ForceSet(v8StringData(field),
                          __createFunction(code, getFunctionCache().size() + 1));
    }

    void V8Scope::rename(const char * from, const char * to) {
        V8_SIMPLE_HEADER;
        v8::Handle<v8::String> f = v8StringData(from);
        v8::Handle<v8::String> t = v8StringData(to);
        _global->ForceSet(t, _global->Get(f));
        _global->ForceSet(f, v8::Undefined());
    }

    int V8Scope::invoke(ScriptingFunction func, const BSONObj* argsObject, const BSONObj* recv,
                        int timeoutMs, bool ignoreReturn, bool readOnlyArgs, bool readOnlyRecv) {
        V8_SIMPLE_HEADER
        v8::Handle<v8::Value> funcValue = _funcs[func-1];
        v8::TryCatch try_catch;
        v8::Local<v8::Value> result;

        // TODO SERVER-8016: properly allocate handles on the stack
        static const int MAX_ARGS = 24;
        const int nargs = argsObject ? argsObject->nFields() : 0;
        uassert(16862, "Too many arguments. Max is 24",
                nargs <= MAX_ARGS);

        v8::Handle<v8::Value> args[MAX_ARGS];
        if (nargs) {
            BSONObjIterator it(*argsObject);
            for (int i=0; i<nargs; i++) {
                BSONElement next = it.next();
                args[i] = mongoToV8Element(next, readOnlyArgs);
            }
        }

        v8::Handle<v8::Object> v8recv;
        if (recv != 0)
            v8recv = mongoToLZV8(*recv, readOnlyRecv);
        else
            v8recv = _global;

        if (!nativeEpilogue()) {
            _error = "JavaScript execution terminated";
            log() << _error << endl;
            uasserted(16711, _error);
        }

        if (timeoutMs)
            // start the deadline timer for this script
            _engine->getDeadlineMonitor()->startDeadline(this, timeoutMs);

        result = ((v8::Function*)(*funcValue))->Call(v8recv, nargs, nargs ? args : NULL);

        if (timeoutMs)
            // stop the deadline timer for this script
            _engine->getDeadlineMonitor()->stopDeadline(this);

        if (!nativePrologue()) {
            _error = "JavaScript execution terminated";
            log() << _error << endl;
            uasserted(16712, _error);
        }

        // throw on error
        checkV8ErrorState(result, try_catch);

        if (!ignoreReturn) {
            v8::Handle<v8::Object> resultObject = result->ToObject();
            // must validate the handle because TerminateExecution may have
            // been thrown after the above checks
            if (!resultObject.IsEmpty() && resultObject->Has(strLitToV8("_v8_function"))) {
                log() << "storing native function as return value" << endl;
                _lastRetIsNativeCode = true;
            }
            else {
                _lastRetIsNativeCode = false;
            }
            _global->ForceSet(strLitToV8("__returnValue"), result);
        }

        return 0;
    }

    bool V8Scope::exec(const StringData& code, const string& name, bool printResult,
                       bool reportError, bool assertOnError, int timeoutMs) {
        V8_SIMPLE_HEADER
        v8::TryCatch try_catch;

        v8::Handle<v8::Script> script =
                v8::Script::Compile(v8::String::New(code.rawData(), code.size()),
                                    v8::String::New(name.c_str(), name.length()));

        if (checkV8ErrorState(script, try_catch, reportError, assertOnError))
            return false;

        if (!nativeEpilogue()) {
            _error = "JavaScript execution terminated";
            if (reportError)
                log() << _error << endl;
            if (assertOnError)
                uasserted(13475, _error);
            return false;
        }

        if (timeoutMs)
            // start the deadline timer for this script
            _engine->getDeadlineMonitor()->startDeadline(this, timeoutMs);

        v8::Handle<v8::Value> result = script->Run();

        if (timeoutMs)
            // stopt the deadline timer for this script
            _engine->getDeadlineMonitor()->stopDeadline(this);

        if (!nativePrologue()) {
            _error = "JavaScript execution terminated";
            if (reportError)
                log() << _error << endl;
            if (assertOnError)
                uasserted(16721, _error);
            return false;
        }

        if (checkV8ErrorState(result, try_catch, reportError, assertOnError))
            return false;

        _global->ForceSet(strLitToV8("__lastres__"), result);

        if (printResult && !result->IsUndefined()) {
            // appears to only be used by shell
            cout << V8String(result) << endl;
        }

        return true;
    }

    void V8Scope::injectNative(const char *field, NativeFunction func, void* data) {
        V8_SIMPLE_HEADER    // required due to public access
        injectNative(field, func, _global, data);
    }

    void V8Scope::injectNative(const char *field, NativeFunction func, v8::Handle<v8::Object>& obj,
                               void* data) {
        v8::Handle<v8::FunctionTemplate> ft = createV8Function(nativeCallback);
        ft->Set(strLitToV8("_native_function"),
                           v8::External::New((void*)func),
                           v8::PropertyAttribute(v8::DontEnum | v8::ReadOnly));
        ft->Set(strLitToV8("_native_data"),
                           v8::External::New(data),
                           v8::PropertyAttribute(v8::DontEnum | v8::ReadOnly));
        injectV8Function(field, ft, obj);
    }

    v8::Handle<v8::FunctionTemplate> V8Scope::injectV8Function(const char *field, v8Function func) {
        return injectV8Function(field, func, _global);
    }

    v8::Handle<v8::FunctionTemplate> V8Scope::injectV8Function(const char *field,
                                                               v8Function func,
                                                               v8::Handle<v8::Object>& obj) {
        return injectV8Function(field, createV8Function(func), obj);
    }

    v8::Handle<v8::FunctionTemplate> V8Scope::injectV8Function(const char *fieldCStr,
                                                               v8::Handle<v8::FunctionTemplate> ft,
                                                               v8::Handle<v8::Object>& obj) {
        v8::Handle<v8::String> field = v8StringData(fieldCStr);
        ft->SetClassName(field);
        v8::Handle<v8::Function> func = ft->GetFunction();
        func->SetName(field);
        obj->ForceSet(field, func);
        return ft;
    }

    v8::Handle<v8::FunctionTemplate> V8Scope::injectV8Method(
            const char *fieldCStr,
            v8Function func,
            v8::Handle<v8::ObjectTemplate>& proto) {
        v8::Handle<v8::String> field = v8StringData(fieldCStr);
        v8::Handle<v8::FunctionTemplate> ft = createV8Function(func);
        v8::Handle<v8::Function> f = ft->GetFunction();
        f->SetName(field);
        proto->Set(field, f);
        return ft;
    }

    v8::Handle<v8::FunctionTemplate> V8Scope::createV8Function(v8Function func) {
        v8::Handle<v8::FunctionTemplate> ft = v8::FunctionTemplate::New(v8Callback);
        ft->Set(strLitToV8("_v8_function"), v8::External::New(reinterpret_cast<void*>(func)),
                static_cast<v8::PropertyAttribute>(v8::DontEnum | v8::ReadOnly));
        return ft;
    }

    void V8Scope::gc() {
        V8_SIMPLE_HEADER
        // trigger low memory notification.  for more granular control over garbage
        // collection cycle, @see v8::V8::IdleNotification.
        v8::V8::LowMemoryNotification();
    }

    void V8Scope::localConnect(const char * dbName) {
        typedef v8::Persistent<v8::FunctionTemplate> FTPtr;
        {
            V8_SIMPLE_HEADER
            if (_connectState == EXTERNAL)
                uasserted(12510, "externalSetup already called, can't call localConnect");
            if (_connectState ==  LOCAL) {
                if (_localDBName == dbName)
                    return;
                uasserted(12511,
                          str::stream() << "localConnect previously called with name "
                                        << _localDBName);
            }

            // NOTE: order is important here.  the following methods must be called after
            //       the above conditional statements.

            // install db access functions in the global object
            installDBAccess();

            // add global load() helper
            injectV8Function("load", load);

            // install the Mongo function object and instantiate the 'db' global
            _MongoFT = FTPtr::New(getMongoFunctionTemplate(this, true));
            injectV8Function("Mongo", MongoFT(), _global);
            execCoreFiles();
            exec("_mongo = new Mongo();", "local connect 2", false, true, true, 0);
            exec((string)"db = _mongo.getDB(\"" + dbName + "\");", "local connect 3",
                 false, true, true, 0);
            _connectState = LOCAL;
            _localDBName = dbName;
        }
        loadStored();
    }

    void V8Scope::externalSetup() {
        typedef v8::Persistent<v8::FunctionTemplate> FTPtr;
        V8_SIMPLE_HEADER
        if (_connectState == EXTERNAL)
            return;
        if (_connectState == LOCAL)
            uasserted(12512, "localConnect already called, can't call externalSetup");

        // install db access functions in the global object
        installDBAccess();

        // install thread-related functions (e.g. _threadInject)
        installFork(this, _global, _context);

        // install 'load' helper function
        injectV8Function("load", load);

        // install the Mongo function object
        _MongoFT = FTPtr::New(getMongoFunctionTemplate(this, false));
        injectV8Function("Mongo", MongoFT(), _global);
        execCoreFiles();
        _connectState = EXTERNAL;
    }

    void V8Scope::installDBAccess() {
        typedef v8::Persistent<v8::FunctionTemplate> FTPtr;
        _DBFT             = FTPtr::New(createV8Function(dbInit));
        _DBQueryFT        = FTPtr::New(createV8Function(dbQueryInit));
        _DBCollectionFT   = FTPtr::New(createV8Function(collectionInit));

        // These must be done before calling injectV8Function
        DBFT()->InstanceTemplate()->SetNamedPropertyHandler(collectionGetter, collectionSetter);
        DBQueryFT()->InstanceTemplate()->SetIndexedPropertyHandler(dbQueryIndexAccess);
        DBCollectionFT()->InstanceTemplate()->SetNamedPropertyHandler(collectionGetter,
                                                                      collectionSetter);

        injectV8Function("DB", DBFT(), _global);
        injectV8Function("DBQuery", DBQueryFT(), _global);
        injectV8Function("DBCollection", DBCollectionFT(), _global);

        // The internal cursor type isn't exposed to the users at all
        _InternalCursorFT = FTPtr::New(getInternalCursorFunctionTemplate(this));
    }

    void V8Scope::installBSONTypes() {
        typedef v8::Persistent<v8::FunctionTemplate> FTPtr;
        _ObjectIdFT  = FTPtr::New(injectV8Function("ObjectId", objectIdInit));
        _DBRefFT     = FTPtr::New(injectV8Function("DBRef", dbRefInit));
        _DBPointerFT = FTPtr::New(injectV8Function("DBPointer", dbPointerInit));

        _BinDataFT    = FTPtr::New(getBinDataFunctionTemplate(this));
        _NumberLongFT = FTPtr::New(getNumberLongFunctionTemplate(this));
        _NumberIntFT  = FTPtr::New(getNumberIntFunctionTemplate(this));
        _TimestampFT  = FTPtr::New(getTimestampFunctionTemplate(this));
        _MinKeyFT     = FTPtr::New(getMinKeyFunctionTemplate(this));
        _MaxKeyFT     = FTPtr::New(getMaxKeyFunctionTemplate(this));

        injectV8Function("BinData", BinDataFT(), _global);
        injectV8Function("NumberLong", NumberLongFT(), _global);
        injectV8Function("NumberInt", NumberIntFT(), _global);
        injectV8Function("Timestamp", TimestampFT(), _global);

        // These are instances created from the functions, not the functions themselves
        _global->ForceSet(strLitToV8("MinKey"), MinKeyFT()->GetFunction()->NewInstance());
        _global->ForceSet(strLitToV8("MaxKey"), MaxKeyFT()->GetFunction()->NewInstance());

        // These all create BinData objects so we don't need to hold on to them.
        injectV8Function("UUID", uuidInit);
        injectV8Function("MD5", md5Init);
        injectV8Function("HexData", hexDataInit);

        _global->Get(strLitToV8("Object"))->ToObject()->ForceSet(
                            strLitToV8("bsonsize"),
                            createV8Function(bsonsize)->GetFunction());
    }


    // ----- internal -----

    void V8Scope::reset() {
        V8_SIMPLE_HEADER
        unregisterOpId();
        _error = "";
        _pendingKill = false;
        _inNativeExecution = true;
        registerOpId();
    }

    v8::Local<v8::Value> V8Scope::newFunction(const StringData& code) {
        v8::HandleScope handle_scope;
        v8::TryCatch try_catch;
        string codeStr = str::stream() << "____MongoToV8_newFunction_temp = " << code;

        v8::Local<v8::Script> compiled = v8::Script::New(v8::String::New(codeStr.c_str(),
                                                                         codeStr.length()));

        // throw on compile error
        checkV8ErrorState(compiled, try_catch);

        v8::Local<v8::Value> ret = compiled->Run();

        // throw on run/assignment error
        checkV8ErrorState(ret, try_catch);

        return handle_scope.Close(ret);
    }

    v8::Local<v8::Value> V8Scope::newId(const OID &id) {
        v8::HandleScope handle_scope;
        v8::Handle<v8::Function> idCons = ObjectIdFT()->GetFunction();
        v8::Handle<v8::Value> argv[1];
        const string& idString = id.str();
        argv[0] = v8::String::New(idString.c_str(), idString.length());
        return handle_scope.Close(idCons->NewInstance(1, argv));
    }

    /**
     * converts a BSONObj to a Lazy V8 object
     */
    v8::Handle<v8::Object> V8Scope::mongoToLZV8(const BSONObj& m, bool readOnly) {
        if (m.firstElementType() == String && str::equals(m.firstElementFieldName(), "$ref")) {
            BSONObjIterator it(m);
            const BSONElement ref = it.next();
            const BSONElement id = it.next();
            if (id.ok() && str::equals(id.fieldName(), "$id")) {
                v8::Handle<v8::Value> args[] = {
                    mongoToV8Element(ref, readOnly),
                    mongoToV8Element(id, readOnly)
                };
                v8::Local<v8::Object> dbRef = DBRefFT()->GetFunction()->NewInstance(2, args);
                while (it.more()) {
                    BSONElement elem = it.next();
                    dbRef->Set(v8StringData(elem.fieldName()), mongoToV8Element(elem, readOnly));
                }
                return dbRef;
            }
        }

        v8::Handle<v8::FunctionTemplate> templ = readOnly ? ROBsonFT() : LazyBsonFT();
        v8::Handle<v8::Object> o = templ->GetFunction()->NewInstance();
        massert(16496, str::stream() << "V8: NULL Object template instantiated. "
                                     << (v8::V8::IsExecutionTerminating() ?
                                        "v8 execution is terminating." :
                                        "v8 still executing."),
                       *o != NULL);

        wrapBSONObject(o, m, readOnly);
        return o;
    }

    v8::Handle<v8::Value> V8Scope::mongoToV8Element(const BSONElement &elem, bool readOnly) {
        v8::Handle<v8::Value> argv[3];      // arguments for v8 instance constructors
        v8::Local<v8::Object> instance;     // instance of v8 type
        uint64_t nativeUnsignedLong;        // native representation of NumberLong

        switch (elem.type()) {
        case mongo::Code:
            return newFunction(StringData(elem.valuestr(), elem.valuestrsize() - 1));
        case CodeWScope:
            if (!elem.codeWScopeObject().isEmpty())
                log() << "warning: CodeWScope doesn't transfer to db.eval" << endl;
            return newFunction(StringData(elem.codeWScopeCode(), elem.codeWScopeCodeLen() - 1));
        case mongo::Symbol:
        case mongo::String:
            return v8::String::New(elem.valuestr(), elem.valuestrsize() - 1);
        case mongo::jstOID:
            return newId(elem.__oid());
        case mongo::NumberDouble:
        case mongo::NumberInt:
            return v8::Number::New(elem.number());
        case mongo::Array: {
            // NB: This comment may no longer be accurate.
            // for arrays it's better to use non lazy object because:
            // - the lazy array is not a true v8 array and requires some v8 src change
            //   for all methods to work
            // - it made several tests about 1.5x slower
            // - most times when an array is accessed, all its values will be used

            // It is faster to allow the v8::Array to grow than call nFields() on the array
            v8::Handle<v8::Array> array = v8::Array::New();
            int i = 0;
            BSONForEach(subElem, elem.embeddedObject()) {
                array->Set(i++, mongoToV8Element(subElem, readOnly));
            }
            return array;
        }
        case mongo::Object:
            return mongoToLZV8(elem.embeddedObject(), readOnly);
        case mongo::Date:
            return v8::Date::New((double) ((long long)elem.date().millis));
        case mongo::Bool:
            return v8::Boolean::New(elem.boolean());
        case mongo::EOO:
        case mongo::jstNULL:
        case mongo::Undefined: // duplicate sm behavior
            return v8::Null();
        case mongo::RegEx: {
            // TODO parse into a custom type that can support any patterns and flags SERVER-9803
            v8::TryCatch tryCatch;

            v8::Handle<v8::Value> args[] = {
                v8::String::New(elem.regex()),
                v8::String::New(elem.regexFlags())
            };

            v8::Handle<v8::Value> ret = _jsRegExpConstructor->NewInstance(2, args);
            uassert(16863, str::stream() << "Error converting " << elem.toString(false)
                                         << " in field " << elem.fieldName()
                                         << " to a JS RegExp object: "
                                         << toSTLString(tryCatch.Exception()),
                    !tryCatch.HasCaught());

            return ret;
        }
        case mongo::BinData: {
            int len;
            const char *data = elem.binData(len);
            stringstream ss;
            base64::encode(ss, data, len);
            argv[0] = v8::Number::New(elem.binDataType());
            argv[1] = v8::String::New(ss.str().c_str());
            return BinDataFT()->GetFunction()->NewInstance(2, argv);
        }
        case mongo::Timestamp:
            argv[0] = v8::Number::New(elem.timestampTime() / 1000);
            argv[1] = v8::Number::New(elem.timestampInc());
            return TimestampFT()->GetFunction()->NewInstance(2,argv);
        case mongo::NumberLong:
            nativeUnsignedLong = elem.numberLong();
            // values above 2^53 are not accurately represented in JS
            if ((long long)nativeUnsignedLong ==
                (long long)(double)(long long)(nativeUnsignedLong) &&
                    nativeUnsignedLong < 9007199254740992ULL) {
                argv[0] = v8::Number::New((double)(long long)(nativeUnsignedLong));
                return NumberLongFT()->GetFunction()->NewInstance(1, argv);
            }
            else {
                argv[0] = v8::Number::New((double)(long long)(nativeUnsignedLong));
                argv[1] = v8::Integer::New(nativeUnsignedLong >> 32);
                argv[2] = v8::Integer::New((unsigned long)
                                           (nativeUnsignedLong & 0x00000000ffffffff));
                return NumberLongFT()->GetFunction()->NewInstance(3, argv);
            }
        case mongo::MinKey:
            return MinKeyFT()->GetFunction()->NewInstance();
        case mongo::MaxKey:
            return MaxKeyFT()->GetFunction()->NewInstance();
        case mongo::DBRef:
            argv[0] = v8StringData(elem.dbrefNS());
            argv[1] = newId(elem.dbrefOID());
            return DBPointerFT()->GetFunction()->NewInstance(2, argv);
        default:
            massert(16661, str::stream() << "can't handle type: " << elem.type()
                                         << " " << elem.toString(), false);
            break;
        }
        return v8::Undefined();
    }

    void V8Scope::v8ToMongoNumber(BSONObjBuilder& b,
                                  const StringData& elementName,
                                  v8::Handle<v8::Number> value,
                                  BSONObj* originalParent) {
        double val = value->Value();
        // if previous type was integer, keep it
        int intval = static_cast<int>(val);
        if (val == intval && originalParent) {
            // This makes copying an object of numbers O(n**2) :(
            BSONElement elmt = originalParent->getField(elementName);
            if (elmt.type() == mongo::NumberInt) {
                b.append(elementName, intval);
                return;
            }
        }
        b.append(elementName, val);
    }

    void V8Scope::v8ToMongoRegex(BSONObjBuilder& b,
                                 const StringData& elementName,
                                 v8::Handle<v8::RegExp> v8Regex) {
        V8String v8RegexString (v8Regex);
        StringData regex = v8RegexString;
        regex = regex.substr(1);
        StringData r = regex.substr(0 ,regex.rfind('/'));
        StringData o = regex.substr(regex.rfind('/') + 1);
        b.appendRegex(elementName, r, o);
    }

    void V8Scope::v8ToMongoDBRef(BSONObjBuilder& b,
                                 const StringData& elementName,
                                 v8::Handle<v8::Object> obj) {
        verify(DBPointerFT()->HasInstance(obj));
        v8::Local<v8::Value> theid = obj->Get(strLitToV8("id"));
        OID oid = v8ToMongoObjectID(theid->ToObject());
        string ns = toSTLString(obj->Get(strLitToV8("ns")));
        b.appendDBRef(elementName, ns, oid);
    }

    void V8Scope::v8ToMongoBinData(BSONObjBuilder& b,
                                   const StringData& elementName,
                                   v8::Handle<v8::Object> obj) {

        verify(BinDataFT()->HasInstance(obj));
        verify(obj->InternalFieldCount() == 1);
        int len = obj->Get(strLitToV8("len"))->ToInt32()->Value();
        b.appendBinData(elementName,
                        len,
                        mongo::BinDataType(obj->Get(strLitToV8("type"))->ToInt32()->Value()),
                        base64::decode(toSTLString(obj->GetInternalField(0))).c_str());
    }

    OID V8Scope::v8ToMongoObjectID(v8::Handle<v8::Object> obj) {
        verify(ObjectIdFT()->HasInstance(obj));
        const string hexStr = toSTLString(obj->Get(strLitToV8("str")));

        // OID parser doesn't have user-friendly error messages
        uassert(16864, "ObjectID.str must be exactly 24 chars long",
                hexStr.size() == 24);
        uassert(16865, "ObjectID.str must only have hex characters [0-1a-fA-F]",
                count_if(hexStr.begin(), hexStr.end(), ::isxdigit) == 24);

        return OID(hexStr);
    }

    void V8Scope::v8ToMongoObject(BSONObjBuilder& b,
                                  const StringData& elementName,
                                  v8::Handle<v8::Value> value,
                                  int depth,
                                  BSONObj* originalParent) {
        verify(value->IsObject());
        v8::Handle<v8::Object> obj = value.As<v8::Object>();

        if (value->IsRegExp()) {
            v8ToMongoRegex(b, elementName, obj.As<v8::RegExp>());
        } else if (ObjectIdFT()->HasInstance(value)) {
            b.append(elementName, v8ToMongoObjectID(obj));
        } else if (NumberLongFT()->HasInstance(value)) {
            b.append(elementName, numberLongVal(this, obj));
        } else if (NumberIntFT()->HasInstance(value)) {
            b.append(elementName, numberIntVal(this, obj));
        } else if (DBPointerFT()->HasInstance(value)) {
            v8ToMongoDBRef(b, elementName, obj);
        } else if (BinDataFT()->HasInstance(value)) {
            v8ToMongoBinData(b, elementName, obj);
        } else if (TimestampFT()->HasInstance(value)) {
            OpTime ot (obj->Get(strLitToV8("t"))->Uint32Value(),
                       obj->Get(strLitToV8("i"))->Uint32Value());
            b.append(elementName, ot);
        } else if (MinKeyFT()->HasInstance(value)) {
            b.appendMinKey(elementName);
        } else if (MaxKeyFT()->HasInstance(value)) {
            b.appendMaxKey(elementName);
        } else {
            // nested object or array
            BSONObj sub = v8ToMongo(obj, depth);
            b.append(elementName, sub);
        }
    }

    void V8Scope::v8ToMongoElement(BSONObjBuilder & b, const StringData& sname,
                                   v8::Handle<v8::Value> value, int depth,
                                   BSONObj* originalParent) {

        // Null char should be at the end, not in the string
        uassert(16985,
                str::stream() << "JavaScript property (name) contains a null char "
                              << "which is not allowed in BSON. "
                              << originalParent->jsonString(),
                (string::npos == sname.find('\0')) );

        if (value->IsString()) {
            b.append(sname, V8String(value));
            return;
        }
        if (value->IsFunction()) {
            uassert(16716, "cannot convert native function to BSON",
                    !value->ToObject()->Has(strLitToV8("_v8_function")));
            b.appendCode(sname, V8String(value));
            return;
        }
        if (value->IsNumber()) {
            v8ToMongoNumber(b, sname, value.As<v8::Number>(), originalParent);
            return;
        }
        if (value->IsArray()) {
            // Note: can't use BSONArrayBuilder because need to call recursively
            BSONObjBuilder arrBuilder(b.subarrayStart(sname));
            v8::Handle<v8::Array> array = value.As<v8::Array>();
            const int len = array->Length();
            for (int i=0; i < len; i++) {
                const string name = BSONObjBuilder::numStr(i);
                v8ToMongoElement(arrBuilder, name, array->Get(i), depth+1, originalParent);
            }
            return;
        }
        if (value->IsDate()) {
            long long dateval = (long long)(v8::Date::Cast(*value)->NumberValue());
            b.appendDate(sname, Date_t((unsigned long long) dateval));
            return;
        }
        if (value->IsExternal())
            return;
        if (value->IsObject()) {
            v8ToMongoObject(b, sname, value, depth, originalParent);
            return;
        }

        if (value->IsBoolean()) {
            b.appendBool(sname, value->ToBoolean()->Value());
            return;
        }
        else if (value->IsUndefined()) {
            b.appendUndefined(sname);
            return;
        }
        else if (value->IsNull()) {
            b.appendNull(sname);
            return;
        }
        uasserted(16662, str::stream() << "unable to convert JavaScript property to mongo element "
                                   << sname);
    }

    BSONObj V8Scope::v8ToMongo(v8::Handle<v8::Object> o, int depth) {
        BSONObj originalBSON;
        if (LazyBsonFT()->HasInstance(o)) {
            originalBSON = unwrapBSONObj(this, o);
            BSONHolder* holder = unwrapHolder(this, o);
            if (holder && !holder->_modified) {
                // object was not modified, use bson as is
                return originalBSON;
            }
        }

        BSONObjBuilder b;

        // We special case the _id field in top-level objects and move it to the front.
        // This matches other drivers behavior and makes finding the _id field quicker in BSON.
        if (depth == 0) {
            if (o->HasOwnProperty(strLitToV8("_id"))) {
                v8ToMongoElement(b, "_id", o->Get(strLitToV8("_id")), 0, &originalBSON);
            }
        }

        v8::Local<v8::Array> names = o->GetOwnPropertyNames();
        for (unsigned int i=0; i<names->Length(); i++) {
            v8::Local<v8::String> name = names->Get(i)->ToString();

            if (depth == 0 && name->StrictEquals(strLitToV8("_id")))
                continue; // already handled above

            V8String sname(name);
            v8::Local<v8::Value> value = o->Get(name);
            v8ToMongoElement(b, sname, value, depth + 1, &originalBSON);
        }
        return b.obj();
    }

    // --- random utils ----

    static logger::MessageLogDomain* jsPrintLogDomain;
    v8::Handle<v8::Value> V8Scope::Print(V8Scope* scope, const v8::Arguments& args) {
        LogstreamBuilder builder(jsPrintLogDomain, getThreadName(), logger::LogSeverity::Log());
        std::ostream& ss = builder.stream();
        v8::HandleScope handle_scope;
        bool first = true;
        for (int i = 0; i < args.Length(); i++) {
            if (first)
                first = false;
            else
                ss << " ";

            if (args[i].IsEmpty()) {
                // failed to get object to convert
                ss << "[unknown type]";
                continue;
            }
            if (args[i]->IsExternal()) {
                // object is External
                ss << "[mongo internal]";
                continue;
            }

            v8::String::Utf8Value str(args[i]);
            ss << *str;
        }
        ss << "\n";
        return handle_scope.Close(v8::Undefined());
    }

    v8::Handle<v8::Value> V8Scope::Version(V8Scope* scope, const v8::Arguments& args) {
        v8::HandleScope handle_scope;
        return handle_scope.Close(v8::String::New(v8::V8::GetVersion()));
    }

    v8::Handle<v8::Value> V8Scope::GCV8(V8Scope* scope, const v8::Arguments& args) {
        // trigger low memory notification.  for more granular control over garbage
        // collection cycle, @see v8::V8::IdleNotification.
        v8::V8::LowMemoryNotification();
        return v8::Undefined();
    }

    v8::Handle<v8::Value> V8Scope::startCpuProfiler(V8Scope* scope, const v8::Arguments& args) {
        if (args.Length() != 1 || !args[0]->IsString()) {
            return v8AssertionException("startCpuProfiler takes a string argument");
        }
        scope->_cpuProfiler.start(*v8::String::Utf8Value(args[0]->ToString()));
        return v8::Undefined();
    }

    v8::Handle<v8::Value> V8Scope::stopCpuProfiler(V8Scope* scope, const v8::Arguments& args) {
        if (args.Length() != 1 || !args[0]->IsString()) {
            return v8AssertionException("stopCpuProfiler takes a string argument");
        }
        scope->_cpuProfiler.stop(*v8::String::Utf8Value(args[0]->ToString()));
        return v8::Undefined();
    }

    v8::Handle<v8::Value> V8Scope::getCpuProfile(V8Scope* scope, const v8::Arguments& args) {
        if (args.Length() != 1 || !args[0]->IsString()) {
            return v8AssertionException("getCpuProfile takes a string argument");
        }
        return scope->mongoToLZV8(scope->_cpuProfiler.fetch(
                *v8::String::Utf8Value(args[0]->ToString())));
    }

    MONGO_INITIALIZER(JavascriptPrintDomain)(InitializerContext*) {
        jsPrintLogDomain = logger::globalLogManager()->getNamedDomain("javascriptOutput");
        return Status::OK();
    }

} // namespace mongo
