// engine.h

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

#include "mongo/db/jsobj.h"

namespace mongo {
    typedef unsigned long long ScriptingFunction;
    typedef BSONObj (*NativeFunction)(const BSONObj& args, void* data);
    typedef map<string, ScriptingFunction> FunctionCacheMap;

    class DBClientWithCommands;

    struct JSFile {
        const char* name;
        const StringData& source;
    };

    class Scope : boost::noncopyable {
    public:
        Scope();
        virtual ~Scope();

        virtual void reset() = 0;
        virtual void init(const BSONObj* data) = 0;
        void init(const char* data) {
            BSONObj o(data);
            init(&o);
        }

        virtual void localConnect(const char* dbName) = 0;
        virtual void externalSetup() = 0;
        virtual void setLocalDB(const string& localDBName) { _localDBName = localDBName; }
        virtual BSONObj getObject(const char* field) = 0;
        virtual string getString(const char* field) = 0;
        virtual bool getBoolean(const char* field) = 0;
        virtual double getNumber(const char* field) = 0;
        virtual int getNumberInt(const char* field) { return (int)getNumber(field); }
        virtual long long getNumberLongLong(const char* field) {
            return static_cast<long long>(getNumber(field));
        }

        virtual void setElement(const char* field, const BSONElement& e) = 0;
        virtual void setNumber(const char* field, double val) = 0;
        virtual void setString(const char* field, const StringData& val) = 0;
        virtual void setObject(const char* field, const BSONObj& obj, bool readOnly=true) = 0;
        virtual void setBoolean(const char* field, bool val) = 0;
        virtual void setFunction(const char* field, const char* code) = 0;

        virtual int type(const char* field) = 0;

        virtual void append(BSONObjBuilder& builder, const char* fieldName, const char* scopeName);

        virtual void rename(const char* from, const char* to) = 0;

        virtual string getError() = 0;

        virtual bool hasOutOfMemoryException() = 0;

        virtual void installBenchRun();

        virtual bool isKillPending() const = 0;

        virtual void gc() = 0;

        virtual ScriptingFunction createFunction(const char* code);

        /**
         * @return 0 on success
         */
        int invoke(const char* code, const BSONObj* args, const BSONObj* recv, int timeoutMs = 0);

        virtual int invoke(ScriptingFunction func, const BSONObj* args, const BSONObj* recv,
                           int timeoutMs = 0, bool ignoreReturn = false, bool readOnlyArgs = false,
                           bool readOnlyRecv = false) = 0;

        void invokeSafe(ScriptingFunction func, const BSONObj* args, const BSONObj* recv,
                        int timeoutMs = 0, bool ignoreReturn = false, bool readOnlyArgs = false,
                        bool readOnlyRecv = false) {
            int res = invoke(func, args, recv, timeoutMs, ignoreReturn,
                             readOnlyArgs, readOnlyRecv);
            if (res == 0)
                return;
            uasserted(9004, string("invoke failed: ") + getError());
        }

        void invokeSafe(const char* code, const BSONObj* args, const BSONObj* recv,
                        int timeoutMs = 0) {
            if (invoke(code, args, recv, timeoutMs) == 0)
                return;
            uasserted(9005, string("invoke failed: ") + getError());
        }

        virtual void injectNative(const char* field, NativeFunction func, void* data = 0) = 0;

        virtual bool exec(const StringData& code, const string& name, bool printResult,
                          bool reportError, bool assertOnError, int timeoutMs = 0) = 0;

        virtual void execSetup(const StringData& code, const string& name = "setup") {
            exec(code, name, false, true, true, 0);
        }

        void execSetup(const JSFile& file) {
            execSetup(file.source, file.name);
        }

        virtual bool execFile(const string& filename, bool printResult, bool reportError,
                              int timeoutMs = 0);

        void execCoreFiles();

        virtual void loadStored(bool ignoreNotConnected = false);

        /**
         * if any changes are made to .system.js, call this
         * right now its just global - slightly inefficient, but a lot simpler
         */
        static void storedFuncMod();

        static void validateObjectIdString(const string& str);

        /** increments the number of times a scope was used */
        void incTimesUsed() { ++_numTimesUsed; }

        /** gets the number of times a scope was used */
        int getTimesUsed() { return _numTimesUsed; }

        /** return true if last invoke() return'd native code */
        virtual bool isLastRetNativeCode() { return _lastRetIsNativeCode; }

        class NoDBAccess {
            Scope* _s;
        public:
            NoDBAccess(Scope* s) : _s(s) {
            }
            ~NoDBAccess() {
                _s->rename("____db____", "db");
            }
        };
        NoDBAccess disableDBAccess(const char* why) {
            rename("db", "____db____");
            return NoDBAccess(this);
        }

    protected:
        friend class PooledScope;
        virtual FunctionCacheMap& getFunctionCache() { return _cachedFunctions; }
        virtual ScriptingFunction _createFunction(const char* code,
                                                  ScriptingFunction functionNumber = 0) = 0;

        string _localDBName;
        long long _loadedVersion;
        set<string> _storedNames;
        static long long _lastVersion;
        FunctionCacheMap _cachedFunctions;
        int _numTimesUsed;
        bool _lastRetIsNativeCode; // v8 only: set to true if eval'd script returns a native func
    };

    class ScriptEngine : boost::noncopyable {
    public:
        ScriptEngine();
        virtual ~ScriptEngine();

        virtual Scope* newScope() {
            return createScope();
        }

        virtual void runTest() = 0;

        virtual bool utf8Ok() const = 0;

        static void setup();

        /** gets a scope from the pool or a new one if pool is empty
         * @param db The db name
         * @param scopeType A unique id to limit scope sharing.
         *                  This must include authenticated users.
         * @return the scope
         */
        auto_ptr<Scope> getPooledScope(const string& db, const string& scopeType);

        void setScopeInitCallback(void (*func)(Scope&)) { _scopeInitCallback = func; }
        static void setConnectCallback(void (*func)(DBClientWithCommands&)) {
            _connectCallback = func;
        }
        static void runConnectCallback(DBClientWithCommands& c) {
            if (_connectCallback)
                _connectCallback(c);
        }

        // engine implementation may either respond to interrupt events or
        // poll for interrupts.  the interrupt functions must not wait indefinitely on a lock.
        virtual void interrupt(unsigned opId) {}
        virtual void interruptAll() {}
        static void setGetCurrentOpIdCallback(unsigned (*func)()) {
            _getCurrentOpIdCallback = func;
        }
        static bool haveGetCurrentOpIdCallback() { return _getCurrentOpIdCallback; }
        static unsigned getCurrentOpId() {
            massert(13474, "no _getCurrentOpIdCallback", _getCurrentOpIdCallback);
            return _getCurrentOpIdCallback();
        }
        static void setCheckInterruptCallback(const char* (*func)()) {
            _checkInterruptCallback = func;
        }
        static bool haveCheckInterruptCallback() { return _checkInterruptCallback; }
        static const char* checkInterrupt() {
            return _checkInterruptCallback ? _checkInterruptCallback() : "";
        }
        static bool interrupted() {
            const char* r = checkInterrupt();
            return r && r[0];
        }

        static std::string getInterpreterVersionString();

    protected:
        virtual Scope* createScope() = 0;
        void (*_scopeInitCallback)(Scope&);

    private:
        static void (*_connectCallback)(DBClientWithCommands&);
        static const char* (*_checkInterruptCallback)();
        static unsigned (*_getCurrentOpIdCallback)();
    };

    void installGlobalUtils(Scope& scope);
    bool hasJSReturn(const string& s);
    const char* jsSkipWhiteSpace(const char* raw);

    extern ScriptEngine* globalScriptEngine;
}
