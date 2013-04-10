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

    static V8Scope* getScope(v8::Isolate* isolate) {
        return static_cast<V8Scope*>(isolate->GetData());
    }

    /**
     * Unwraps a BSONObj from the JS wrapper
     */
    static BSONObj unwrapBSONObj(const v8::Handle<v8::Object>& obj) {
        v8::Handle<v8::External> field = v8::Handle<v8::External>::Cast(obj->GetInternalField(0));
        if (field.IsEmpty() || !field->IsExternal()) {
            return BSONObj();
        }
        void* ptr = field->Value();
        return ((BSONHolder*)ptr)->_obj;
    }

    static BSONHolder* unwrapHolder(const v8::Handle<v8::Object>& obj) {
      v8::Handle<v8::External> field = v8::Handle<v8::External>::Cast(obj->GetInternalField(0));
      if (field.IsEmpty() || !field->IsExternal())
          return 0;
      void* ptr = field->Value();
      return (BSONHolder*)ptr;
    }

    static v8::Handle<v8::Object> unwrapObject(const v8::Handle<v8::Object>& obj) {
      return obj->GetInternalField(1).As<v8::Object>();
    }

    v8::Persistent<v8::Object> V8Scope::wrapBSONObject(v8::Local<v8::Object> obj,
                                                       BSONHolder* data) {
        data->_scope = this;
        obj->SetInternalField(0, v8::External::New(data)); // Holder
        obj->SetInternalField(1, v8::Object::New()); // Object
        v8::Persistent<v8::Object> p = v8::Persistent<v8::Object>::New(obj);
        bsonHolderTracker.track(p, data);
        return p;
    }

    static v8::Handle<v8::Value> namedGet(v8::Local<v8::String> name,
                                          const v8::AccessorInfo& info) {
        v8::HandleScope handle_scope;
        v8::Handle<v8::Value> val;
        try {
            v8::Handle<v8::Object> realObject = unwrapObject(info.Holder());
            if (realObject->HasOwnProperty(name)) {
                // value already cached or added
                return handle_scope.Close(realObject->Get(name));
            }

            string key = toSTLString(name);
            BSONHolder* holder = unwrapHolder(info.Holder());
            if (!holder || holder->_removed.count(key))
                return handle_scope.Close(v8::Handle<v8::Value>());

            BSONObj obj = holder->_obj;
            BSONElement elmt = obj.getField(key.c_str());
            if (elmt.eoo())
                return handle_scope.Close(v8::Handle<v8::Value>());

            V8Scope* scope = getScope(info.GetIsolate());
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
        // Rest of function is unused but left in to ease backporting of SERVER-9267

        v8::HandleScope handle_scope;
        v8::Handle<v8::Value> val;
        string key = toSTLString(name);
        try {
            BSONObj obj = unwrapBSONObj(info.Holder());
            BSONElement elmt = obj.getField(key.c_str());
            if (elmt.eoo())
                return handle_scope.Close(v8::Handle<v8::Value>());
            V8Scope* scope = getScope(info.GetIsolate());
            val = scope->mongoToV8Element(elmt, true);
        }
        catch (const DBException &dbEx) {
            return v8AssertionException(dbEx.toString());
        }
        catch (...) {
            return v8AssertionException(string("error getting read-only property ") + key);
        }
        return handle_scope.Close(val);
    }

    static v8::Handle<v8::Value> namedSet(v8::Local<v8::String> name,
                                          v8::Local<v8::Value> value_obj,
                                          const v8::AccessorInfo& info) {
        string key = toSTLString(name);
        BSONHolder* holder = unwrapHolder(info.Holder());
        if (!holder) return v8::Handle<v8::Value>();
        holder->_removed.erase(key);
        holder->_modified = true;

        v8::Handle<v8::Object> realObject = unwrapObject(info.Holder());
        realObject->Set(name, value_obj);
        return value_obj;
    }

    static v8::Handle<v8::Array> namedEnumerator(const v8::AccessorInfo &info) {
        v8::HandleScope handle_scope;
        BSONHolder* holder = unwrapHolder(info.Holder());
        if (!holder) return v8::Handle<v8::Array>();
        BSONObj obj = holder->_obj;
        v8::Handle<v8::Array> out = v8::Array::New();
        int outIndex = 0;
        V8Scope* scope = getScope(info.GetIsolate());

        unordered_set<string> added;
        // note here that if keys are parseable number, v8 will access them using index
        for (BSONObjIterator it(obj); it.more();) {
            const BSONElement& f = it.next();
            string sname = f.fieldName();
            if (holder->_removed.count(sname))
                continue;

            v8::Handle<v8::String> name = scope->v8StringData(sname);
            added.insert(sname);
            out->Set(outIndex++, name);
        }


        v8::Handle<v8::Object> realObject = unwrapObject(info.Holder());
        v8::Handle<v8::Array> fields = realObject->GetOwnPropertyNames();
        const int len = fields->Length();
        for (int field=0; field < len; field++) {
            v8::Handle<v8::String> name = fields->Get(field).As<v8::String>();
            string sname = toSTLString(name);
            if (added.count(sname))
                continue;
            out->Set(outIndex++, name);
        }
        return handle_scope.Close(out);
    }

    v8::Handle<v8::Boolean> namedDelete(v8::Local<v8::String> name, const v8::AccessorInfo& info) {
        v8::HandleScope handle_scope;
        string key = toSTLString(name);
        BSONHolder* holder = unwrapHolder(info.Holder());
        if (!holder) return v8::Handle<v8::Boolean>();
        holder->_removed.insert(key);
        holder->_modified = true;

        v8::Handle<v8::Object> realObject = unwrapObject(info.Holder());
        realObject->Delete(name);
        return v8::True();
    }

    static v8::Handle<v8::Value> indexedGet(uint32_t index, const v8::AccessorInfo &info) {
        v8::HandleScope handle_scope;
        v8::Handle<v8::Value> val;
        try {
            v8::Handle<v8::Object> realObject = unwrapObject(info.Holder());
            if (realObject->Has(index)) {
                // value already cached or added
                return handle_scope.Close(realObject->Get(index));
            }
            string key = str::stream() << index;
            V8Scope* scope = getScope(info.GetIsolate());

            BSONHolder* holder = unwrapHolder(info.Holder());
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
        BSONHolder* holder = unwrapHolder(info.Holder());
        if (!holder) return v8::Handle<v8::Boolean>();
        holder->_removed.insert(key);
        holder->_modified = true;

        // also delete in JS obj
        v8::Handle<v8::Object> realObject = unwrapObject(info.Holder());
        realObject->Delete(index);
        return v8::True();
    }

    static v8::Handle<v8::Value> indexedGetRO(uint32_t index, const v8::AccessorInfo &info) {
        return indexedGet(index, info);
        // Rest of function is unused but left in-place to ease backporting of SERVER-9267

        v8::HandleScope handle_scope;
        v8::Handle<v8::Value> val;
        try {
            string key = str::stream() << index;
            V8Scope* scope = getScope(info.GetIsolate());

            BSONObj obj = unwrapBSONObj(info.Holder());
            BSONElement elmt = obj.getField(key);

            if (elmt.eoo())
                return handle_scope.Close(v8::Handle<v8::Value>());

            val = scope->mongoToV8Element(elmt, true);
        }
        catch (const DBException &dbEx) {
            return v8AssertionException(dbEx.toString());
        }
        catch (...) {
            return v8AssertionException(str::stream()
                                            << "error getting read-only indexed property "
                                            << index);
        }
        return handle_scope.Close(val);
    }

    static v8::Handle<v8::Value> indexedSet(uint32_t index, v8::Local<v8::Value> value_obj,
                                            const v8::AccessorInfo& info) {
        string key = str::stream() << index;
        BSONHolder* holder = unwrapHolder(info.Holder());
        if (!holder) return v8::Handle<v8::Value>();
        holder->_removed.erase(key);
        holder->_modified = true;

        v8::Handle<v8::Object> realObject = unwrapObject(info.Holder());
        realObject->Set(index, value_obj);
        return value_obj;
    }

    v8::Handle<v8::Value> NamedReadOnlySet(v8::Local<v8::String> property,
                                           v8::Local<v8::Value> value,
                                           const v8::AccessorInfo& info) {
        string key = toSTLString(property);
        cout << "cannot write property " << key << " to read-only object" << endl;
        return value;
    }

    v8::Handle<v8::Boolean> NamedReadOnlyDelete(v8::Local<v8::String> property,
                                                const v8::AccessorInfo& info) {
        string key = toSTLString(property);
        cout << "cannot delete property " << key << " from read-only object" << endl;
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
        const int verbosity = 1;    // log level for stat collection
        if (logLevel < verbosity)
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
        LOG(2) << "V8Scope " << this << " registered for op " << _opId << endl;
    }

    void V8Scope::unregisterOpId() {
        scoped_lock giLock(_engine->_globalInterruptLock);
        LOG(2) << "V8Scope " << this << " unregistered for op " << _opId << endl;
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
            LOG(2) << "v8 execution interrupted.  isolate: " << _isolate << endl;
            return false;
        }
        if (_pendingKill || globalScriptEngine->interrupted()) {
            // kill flag was set before entering our callback
            LOG(2) << "marked for death while leaving callback.  isolate: " << _isolate << endl;
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
            LOG(2) << "v8 execution interrupted.  isolate: " << _isolate << endl;
            return false;
        }
        if (_pendingKill || globalScriptEngine->interrupted()) {
            LOG(2) << "marked for death while leaving callback.  isolate: " << _isolate << endl;
            v8::V8::TerminateExecution(_isolate);
            return false;
        }
        return true;
    }

    void V8Scope::kill() {
        mongo::mutex::scoped_lock interruptLock(_interruptLock);
        if (!_inNativeExecution) {
            // set the TERMINATE flag on the stack guard for this isolate
            v8::V8::TerminateExecution(_isolate);
            LOG(1) << "killing v8 scope.  isolate: " << _isolate << endl;
        }
        LOG(1) << "marking v8 scope for death.  isolate: " << _isolate << endl;
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
        if (logLevel > 1) {
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

        // initialize lazy object template
        lzObjectTemplate = v8::Persistent<v8::ObjectTemplate>::New(v8::ObjectTemplate::New());
        lzObjectTemplate->SetInternalFieldCount(2);
        lzObjectTemplate->SetNamedPropertyHandler(namedGet, namedSet, 0, namedDelete,
                                                  namedEnumerator);
        lzObjectTemplate->SetIndexedPropertyHandler(indexedGet, indexedSet, 0, indexedDelete,
                                                    namedEnumerator);
        lzObjectTemplate->NewInstance()->GetPrototype()->ToObject()->ForceSet(
                                                                         v8::String::New("_bson"),
                                                                         v8::Boolean::New(true),
                                                                         v8::DontEnum);

        roObjectTemplate = v8::Persistent<v8::ObjectTemplate>::New(v8::ObjectTemplate::New());
        roObjectTemplate->SetInternalFieldCount(2);
        roObjectTemplate->SetNamedPropertyHandler(namedGetRO, NamedReadOnlySet, 0,
                                                  NamedReadOnlyDelete, namedEnumerator);
        roObjectTemplate->SetIndexedPropertyHandler(indexedGetRO, IndexedReadOnlySet, 0,
                                                    IndexedReadOnlyDelete, 0);
        roObjectTemplate->NewInstance()->GetPrototype()->ToObject()->ForceSet(
                                                                         v8::String::New("_bson"),
                                                                         v8::Boolean::New(true),
                                                                         v8::DontEnum);

        // initialize lazy array template
        // unfortunately it is not possible to create true v8 array from a template
        // this means we use an object template and copy methods over
        // this it creates issues when calling certain methods that check array type
        lzArrayTemplate = v8::Persistent<v8::ObjectTemplate>::New(v8::ObjectTemplate::New());
        lzArrayTemplate->SetInternalFieldCount(1);
        lzArrayTemplate->SetIndexedPropertyHandler(indexedGet, 0, 0, 0, 0);
        lzArrayTemplate->NewInstance()->GetPrototype()->ToObject()->ForceSet(
                                                                        v8::String::New("_bson"),
                                                                        v8::Boolean::New(true),
                                                                        v8::DontEnum);

        internalFieldObjects = v8::Persistent<v8::ObjectTemplate>::New(v8::ObjectTemplate::New());
        internalFieldObjects->SetInternalFieldCount(1);

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
            lzObjectTemplate.Dispose();
            lzArrayTemplate.Dispose();
            roObjectTemplate.Dispose();
            internalFieldObjects.Dispose();
            _context.Dispose();
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
            if (!scope->execFile(filename, false, true, false)) {
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
                    v8::External::Cast(*args.Callee()->Get(v8::String::New("_native_function")));
            NativeFunction function = (NativeFunction)(f->Value());
            v8::Local<v8::External> data =
                    v8::External::Cast(*args.Callee()->Get(v8::String::New("_native_data")));
            BSONObjBuilder b;
            for (int i = 0; i < args.Length(); ++i)
                scope->v8ToMongoElement(b, str::stream() << i, args[i]);
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
                v8::External::Cast(*args.Callee()->Get(v8::String::New("_v8_function")));
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
        v8::Local<v8::Template> proto = numberLong->PrototypeTemplate();
        scope->injectV8Function("valueOf", numberLongValueOf, proto);
        scope->injectV8Function("toNumber", numberLongToNumber, proto);
        scope->injectV8Function("toString", numberLongToString, proto);
        return numberLong;
    }

    v8::Handle<v8::FunctionTemplate> getNumberIntFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> numberInt = scope->createV8Function(numberIntInit);
        v8::Local<v8::Template> proto = numberInt->PrototypeTemplate();
        scope->injectV8Function("valueOf", numberIntValueOf, proto);
        scope->injectV8Function("toNumber", numberIntToNumber, proto);
        scope->injectV8Function("toString", numberIntToString, proto);
        return numberInt;
    }

    v8::Handle<v8::FunctionTemplate> getBinDataFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> binData = scope->createV8Function(binDataInit);
        binData->InstanceTemplate()->SetInternalFieldCount(1);
        v8::Local<v8::Template> proto = binData->PrototypeTemplate();
        scope->injectV8Function("toString", binDataToString, proto);
        scope->injectV8Function("base64", binDataToBase64, proto);
        scope->injectV8Function("hex", binDataToHex, proto);
        return binData;
    }

    v8::Handle<v8::FunctionTemplate> getTimestampFunctionTemplate(V8Scope* scope) {
        v8::Handle<v8::FunctionTemplate> ts = scope->createV8Function(dbTimestampInit);
        ts->InstanceTemplate()->SetInternalFieldCount(1);
        return ts;
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
            if (linenum != 1) ss << ":L" << linenum;
        }
        return ss.str();
    }

    // --- functions -----

    bool hasFunctionIdentifier(const string& code) {
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
        v8::Handle<v8::Value> args[24];

        const int nargs = argsObject ? argsObject->nFields() : 0;
        if (nargs) {
            BSONObjIterator it(*argsObject);
            for (int i=0; i<nargs && i<24; i++) {
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
            if (!resultObject.IsEmpty() && resultObject->Has(v8StringData("_v8_function"))) {
                log() << "storing native function as return value" << endl;
                _lastRetIsNativeCode = true;
            }
            else {
                _lastRetIsNativeCode = false;
            }
            _global->ForceSet(v8::String::New("__returnValue"), result);
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

        _global->ForceSet(v8StringData("__lastres__"), result);

        if (printResult && !result->IsUndefined()) {
            // appears to only be used by shell
            cout << toSTLString(result) << endl;
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
        ft->Set(v8::String::New("_native_function"), v8::External::New((void*)func));
        ft->Set(v8::String::New("_native_data"), v8::External::New(data));
        ft->SetClassName(v8StringData(field));
        obj->ForceSet(v8StringData(field), ft->GetFunction());
    }

    void V8Scope::injectV8Function(const char *field, v8Function func) {
        injectV8Function(field, func, _global);
    }

    void V8Scope::injectV8Function(const char *field, v8Function func,
                                   v8::Handle<v8::Object>& obj) {
        v8::Handle<v8::FunctionTemplate> ft = createV8Function(func);
        ft->SetClassName(v8StringData(field));
        v8::Handle<v8::Function> f = ft->GetFunction();
        obj->ForceSet(v8StringData(field), f);
    }

    void V8Scope::injectV8Function(const char *field, v8Function func,
                                   v8::Handle<v8::Template>& t) {
        v8::Handle<v8::FunctionTemplate> ft = createV8Function(func);
        ft->SetClassName(v8StringData(field));
        v8::Handle<v8::Function> f = ft->GetFunction();
        t->Set(v8StringData(field), f);
    }

    v8::Handle<v8::FunctionTemplate> V8Scope::createV8Function(v8Function func) {
        v8::Handle<v8::FunctionTemplate> ft = v8::FunctionTemplate::New(v8Callback);
        ft->Set(v8::String::New("_v8_function"), v8::External::New(reinterpret_cast<void*>(func)),
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
            _global->ForceSet(v8StringData("Mongo"),
                              getMongoFunctionTemplate(this, true)->GetFunction());
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
        _global->ForceSet(v8StringData("Mongo"),
                          getMongoFunctionTemplate(this, false)->GetFunction());
        execCoreFiles();
        _connectState = EXTERNAL;
    }

    void V8Scope::installDBAccess() {
        v8::Handle<v8::FunctionTemplate> db = createV8Function(dbInit);
        db->InstanceTemplate()->SetNamedPropertyHandler(collectionGetter, collectionSetter);
        _global->ForceSet(v8StringData("DB"), db->GetFunction());

        v8::Handle<v8::FunctionTemplate> dbCollection = createV8Function(collectionInit);
        dbCollection->InstanceTemplate()->SetNamedPropertyHandler(collectionGetter,
                                                                  collectionSetter);
        _global->ForceSet(v8StringData("DBCollection"), dbCollection->GetFunction());

        v8::Handle<v8::FunctionTemplate> dbQuery = createV8Function(dbQueryInit);
        dbQuery->InstanceTemplate()->SetIndexedPropertyHandler(dbQueryIndexAccess);
        _global->ForceSet(v8StringData("DBQuery"), dbQuery->GetFunction());
    }

    void V8Scope::installBSONTypes() {
        injectV8Function("ObjectId", objectIdInit, _global);
        injectV8Function("DBRef", dbRefInit, _global);
        injectV8Function("DBPointer", dbPointerInit, _global);

        _global->ForceSet(v8StringData("BinData"),
                          getBinDataFunctionTemplate(this)->GetFunction());
        _global->ForceSet(v8StringData("UUID"),
                          createV8Function(uuidInit)->GetFunction());
        _global->ForceSet(v8StringData("MD5"),
                          createV8Function(md5Init)->GetFunction());
        _global->ForceSet(v8StringData("HexData"),
                          createV8Function(hexDataInit)->GetFunction());
        _global->ForceSet(v8StringData("NumberLong"),
                          getNumberLongFunctionTemplate(this)->GetFunction());
        _global->ForceSet(v8StringData("NumberInt"),
                          getNumberIntFunctionTemplate(this)->GetFunction());
        _global->ForceSet(v8StringData("Timestamp"),
                          getTimestampFunctionTemplate(this)->GetFunction());

        BSONObjBuilder b;
        b.appendMaxKey("");
        b.appendMinKey("");
        BSONObj o = b.obj();
        BSONObjIterator i(o);
        _global->ForceSet(v8StringData("MaxKey"), mongoToV8Element(i.next()), v8::ReadOnly);
        _global->ForceSet(v8StringData("MinKey"), mongoToV8Element(i.next()), v8::ReadOnly);
        _global->Get(v8StringData("Object"))->ToObject()->ForceSet(
                            v8StringData("bsonsize"),
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
        v8::Function* idCons = this->getObjectIdCons();
        v8::Handle<v8::Value> argv[1];
        const string& idString = id.str();
        argv[0] = v8::String::New(idString.c_str(), idString.length());
        return handle_scope.Close(idCons->NewInstance(1, argv));
    }

    /**
     * converts a BSONObj to a Lazy V8 object
     */
    v8::Persistent<v8::Object> V8Scope::mongoToLZV8(const BSONObj& m, bool readOnly) {
        v8::Local<v8::Object> o;
        BSONHolder* own = new BSONHolder(m);
        own->_readOnly = readOnly;

        if (readOnly) {
            o = roObjectTemplate->NewInstance();
            massert(16497, str::stream() << "V8: NULL RO Object template instantiated. "
                                         << (v8::V8::IsExecutionTerminating() ?
                                            "v8 execution is terminating." :
                                            "v8 still executing."),
                           *o != NULL);
        } else {
            o = lzObjectTemplate->NewInstance();
            massert(16496, str::stream() << "V8: NULL Object template instantiated. "
                                         << (v8::V8::IsExecutionTerminating() ?
                                            "v8 execution is terminating." :
                                            "v8 still executing."),
                           *o != NULL);
            static string ref = "$ref";
            if (ref == m.firstElement().fieldName()) {
                const BSONElement& id = m["$id"];
                if (!id.eoo()) {
                    v8::Function* dbRef = getNamedCons("DBRef");
                    o->SetPrototype(dbRef->NewInstance()->GetPrototype());
                }
            }
        }

        return wrapBSONObject(o, own);

    }

    v8::Handle<v8::Value> minKeyToJson(const v8::Arguments& args) {
        return v8::String::New("{ \"$minKey\" : 1 }");
    }

    v8::Handle<v8::Value> minKeyToString(const v8::Arguments& args) {
        return v8::String::New("[object MinKey]");
    }

    v8::Local<v8::Object> V8Scope::newMinKeyInstance() {
        v8::Local<v8::ObjectTemplate> myTemplate = v8::Local<v8::ObjectTemplate>::New(
                v8::ObjectTemplate::New());
        myTemplate->SetInternalFieldCount(1);
        myTemplate->SetCallAsFunctionHandler(minKeyToJson);

        v8::Local<v8::Object> instance = myTemplate->NewInstance();
        instance->ForceSet(v8::String::New("tojson"),
                           v8::FunctionTemplate::New(minKeyToJson)->GetFunction(), v8::ReadOnly);
        instance->ForceSet(v8::String::New("toString"),
                           v8::FunctionTemplate::New(minKeyToJson)->GetFunction(), v8::ReadOnly);
        instance->SetInternalField(0, v8::Uint32::New( mongo::MinKey ));
        return instance;
    }

    v8::Handle<v8::Value> maxKeyToJson(const v8::Arguments& args) {
        return v8::String::New("{ \"$maxKey\" : 1 }");
    }

    v8::Handle<v8::Value> maxKeyToString(const v8::Arguments& args) {
        return v8::String::New("[object MaxKey]");
    }

    v8::Local<v8::Object> V8Scope::newMaxKeyInstance() {
        v8::Local<v8::ObjectTemplate> myTemplate = v8::Local<v8::ObjectTemplate>::New(
                v8::ObjectTemplate::New());
        myTemplate->SetInternalFieldCount(1);
        myTemplate->SetCallAsFunctionHandler(maxKeyToJson);

        v8::Local<v8::Object> instance = myTemplate->NewInstance();
        instance->ForceSet(v8::String::New("tojson"),
                           v8::FunctionTemplate::New(maxKeyToJson)->GetFunction(), v8::ReadOnly);
        instance->ForceSet(v8::String::New("toString"),
                           v8::FunctionTemplate::New(maxKeyToJson)->GetFunction(), v8::ReadOnly);
        instance->SetInternalField(0, v8::Uint32::New( mongo::MaxKey ));
        return instance;
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
        case mongo::RegEx:
            argv[0] = v8::String::New(elem.regex());
            argv[1] = v8::String::New(elem.regexFlags());
            return getNamedCons("RegExp")->NewInstance(2, argv);
        case mongo::BinData: {
            int len;
            const char *data = elem.binData(len);
            stringstream ss;
            base64::encode(ss, data, len);
            argv[0] = v8::Number::New(elem.binDataType());
            argv[1] = v8::String::New(ss.str().c_str());
            return getNamedCons("BinData")->NewInstance(2, argv);
        }
        case mongo::Timestamp:
            argv[0] = v8::Number::New(elem.timestampTime() / 1000);
            argv[1] = v8::Number::New(elem.timestampInc());
            return getNamedCons("Timestamp")->NewInstance(2,argv);
        case mongo::NumberLong:
            nativeUnsignedLong = elem.numberLong();
            // values above 2^53 are not accurately represented in JS
            if ((long long)nativeUnsignedLong ==
                (long long)(double)(long long)(nativeUnsignedLong) &&
                    nativeUnsignedLong < 9007199254740992ULL) {
                argv[0] = v8::Number::New((double)(long long)(nativeUnsignedLong));
                return getNamedCons("NumberLong")->NewInstance(1, argv);
            }
            else {
                argv[0] = v8::Number::New((double)(long long)(nativeUnsignedLong));
                argv[1] = v8::Integer::New(nativeUnsignedLong >> 32);
                argv[2] = v8::Integer::New((unsigned long)
                                           (nativeUnsignedLong & 0x00000000ffffffff));
                return getNamedCons("NumberLong")->NewInstance(3, argv);
            }
        case mongo::MinKey:
            return newMinKeyInstance();
        case mongo::MaxKey:
            return newMaxKeyInstance();
        case mongo::DBRef:
            argv[0] = v8StringData(elem.dbrefNS());
            argv[1] = newId(elem.dbrefOID());
            return getNamedCons("DBPointer")->NewInstance(2, argv);
        default:
            massert(16661, str::stream() << "can't handle type: " << elem.type()
                                         << " " << elem.toString(), false);
            break;
        }
        return v8::Undefined();
    }

    void V8Scope::v8ToMongoNumber(BSONObjBuilder& b,
                                  const string& elementName,
                                  v8::Handle<v8::Value> value,
                                  BSONObj* originalParent) {
        double val = value->ToNumber()->Value();
        // if previous type was integer, keep it
        int intval = static_cast<int>(val);
        if (val == intval && originalParent) {
            BSONElement elmt = originalParent->getField(elementName);
            if (elmt.type() == mongo::NumberInt) {
                b.append(elementName, intval);
                return;
            }
        }
        b.append(elementName, val);
    }

    void V8Scope::v8ToMongoNumberLong(BSONObjBuilder& b,
                                      const string& elementName,
                                      v8::Handle<v8::Object> obj) {
        // TODO might be nice to potentially speed this up with an indexed internal
        // field, but I don't yet know how to use an ObjectTemplate with a
        // constructor.
        long long val;
        if (!obj->Has(v8StringData("top"))) {
            val = static_cast<int64_t>(obj->Get(v8StringData("floatApprox"))->NumberValue());
        }
        else {
            val = static_cast<int64_t>((
                    static_cast<uint64_t>(obj->Get(v8StringData("top"))->ToInt32()->Value()) << 32) +
                    static_cast<uint32_t>(obj->Get(v8StringData("bottom"))->ToInt32()->Value()));
        }
        b.append(elementName, val);
    }

    void V8Scope::v8ToMongoInternal(BSONObjBuilder& b,
                                    const string& elementName,
                                    v8::Handle<v8::Object> obj) {
        uint32_t bsonTypeRaw = obj->GetInternalField(0)->ToUint32()->Value();
        BSONType bsonType = static_cast<BSONType>(bsonTypeRaw);
        switch(bsonType) {
        case Timestamp:
            b.appendTimestamp(elementName,
                              Date_t(static_cast<uint64_t>(
                                    obj->Get(v8::String::New("t"))->ToNumber()->Value() * 1000 )),
                              obj->Get(v8::String::New("i"))->ToInt32()->Value());
            return;
        case MinKey:
            b.appendMinKey(elementName);
            return;
        case MaxKey:
            b.appendMaxKey(elementName);
            return;
        default:
            massert(16665, "invalid internal field", false);
        }
    }

    void V8Scope::v8ToMongoRegex(BSONObjBuilder& b,
                                 const string& elementName,
                                 v8::Handle<v8::Object> v8Regex) {
        string regex = toSTLString(v8Regex);
        regex = regex.substr(1);
        string r = regex.substr(0 ,regex.rfind("/"));
        string o = regex.substr(regex.rfind("/") + 1);
        b.appendRegex(elementName, r, o);
    }

    void V8Scope::v8ToMongoDBRef(BSONObjBuilder& b,
                                 const string& elementName,
                                 v8::Handle<v8::Object> obj) {
        OID oid;
        v8::Local<v8::Value> theid = obj->Get(v8StringData("id"));
        oid.init(toSTLString(theid->ToObject()->Get(v8StringData("str"))));
        string ns = toSTLString(obj->Get(v8StringData("ns")));
        b.appendDBRef(elementName, ns, oid);
    }

    void V8Scope::v8ToMongoBinData(BSONObjBuilder& b,
                                   const string& elementName,
                                   v8::Handle<v8::Object> obj) {
        int len = obj->Get(v8StringData("len"))->ToInt32()->Value();
        b.appendBinData(elementName,
                        len,
                        mongo::BinDataType(obj->Get(v8StringData("type"))->ToInt32()->Value()),
                        base64::decode(toSTLString(obj->GetInternalField(0))).c_str());
    }

    void V8Scope::v8ToMongoObjectID(BSONObjBuilder& b,
                                    const string& elementName,
                                    v8::Handle<v8::Object> obj) {
        OID oid;
        oid.init(toSTLString(obj->Get(v8StringData("str"))));
        b.appendOID(elementName, &oid);
    }

    void V8Scope::v8ToMongoObject(BSONObjBuilder& b,
                                  const string& elementName,
                                  v8::Handle<v8::Value> value,
                                  int depth,
                                  BSONObj* originalParent) {
        // The user could potentially modify the fields of these special objects,
        // wreaking havoc when we attempt to reinterpret them.  Not doing any validation
        // for now...
        v8::Local<v8::Object> obj = value->ToObject();
        v8::Local<v8::Value> proto = obj->GetPrototype();

        if (obj->InternalFieldCount() && obj->GetInternalField(0)->IsNumber()) {
            v8ToMongoInternal(b, elementName, obj);
            return;
        }

        if (proto->IsRegExp())
            v8ToMongoRegex(b, elementName, obj);
        else if (proto->IsObject() &&
                 proto->ToObject()->HasRealNamedProperty(v8::String::New("isObjectId")))
            v8ToMongoObjectID(b, elementName, obj);
        else if (!obj->GetHiddenValue(v8::String::New("__NumberLong")).IsEmpty())
            v8ToMongoNumberLong(b, elementName, obj);
        else if (!obj->GetHiddenValue(v8::String::New("__NumberInt")).IsEmpty())
            b.append(elementName,
                     obj->GetHiddenValue(v8::String::New("__NumberInt"))->Int32Value());
        else if (!value->ToObject()->GetHiddenValue(v8::String::New("__DBPointer")).IsEmpty())
            v8ToMongoDBRef(b, elementName, obj);
        else if (!value->ToObject()->GetHiddenValue(v8::String::New("__BinData")).IsEmpty())
            v8ToMongoBinData(b, elementName, obj);
        else {
            // nested object or array
            BSONObj sub = v8ToMongo(obj, depth);
            b.append(elementName, sub);
        }
    }

    void V8Scope::v8ToMongoElement(BSONObjBuilder & b, const string& sname,
                                   v8::Handle<v8::Value> value, int depth,
                                   BSONObj* originalParent) {
        if (value->IsString()) {
            b.append(sname, toSTLString(value));
            return;
        }
        if (value->IsFunction()) {
            uassert(16716, "cannot convert native function to BSON",
                    !value->ToObject()->Has(v8StringData("_v8_function")));
            b.appendCode(sname, toSTLString(value));
            return;
        }
        if (value->IsNumber()) {
            v8ToMongoNumber(b, sname, value, originalParent);
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
        if (o->Has(v8::String::New("_bson"))) {
            originalBSON = unwrapBSONObj(o);
            BSONHolder* holder = unwrapHolder(o);
            if (holder && !holder->_modified) {
                // object was not modified, use bson as is
                return originalBSON;
            }
        }

        BSONObjBuilder b;

        // We special case the _id field in top-level objects and move it to the front.
        // This matches other drivers behavior and makes finding the _id field quicker in BSON.
        if (depth == 0) {
            if (o->HasOwnProperty(v8::String::New("_id"))) {
                v8ToMongoElement(b, "_id", o->Get(v8::String::New("_id")), 0, &originalBSON);
            }
        }

        v8::Local<v8::Array> names = o->GetOwnPropertyNames();
        for (unsigned int i=0; i<names->Length(); i++) {
            v8::Local<v8::String> name = names->Get(i)->ToString();
            v8::Local<v8::Value> value = o->Get(name);
            const string sname = toSTLString(name);
            if (depth == 0 && sname == "_id")
                continue; // already handled above

            v8ToMongoElement(b, sname, value, depth + 1, &originalBSON);
        }
        return b.obj();
    }

    // --- random utils ----

    v8::Function * V8Scope::getNamedCons(const char * name) {
        return v8::Function::Cast(*(v8::Context::GetCurrent()->Global()->Get(v8StringData(name))));
    }

    v8::Function * V8Scope::getObjectIdCons() {
        return getNamedCons("ObjectId");
    }

    v8::Handle<v8::Value> V8Scope::Print(V8Scope* scope, const v8::Arguments& args) {
        stringstream ss;
        v8::HandleScope handle_scope;
        bool first = true;
        for (int i = 0; i < args.Length(); i++) {
            if (first)
                first = false;
            else
                ss << " ";

            if (!*args[i]) {
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
        Logstream::logLockless(ss.str());
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

} // namespace mongo
