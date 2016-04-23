// engine.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/scripting/engine.h"

#include <boost/filesystem/operations.hpp>
#include <cctype>

#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/text.h"

namespace mongo {

using std::endl;
using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

AtomicInt64 Scope::_lastVersion(1);

namespace {
// 2 GB is the largest support Javascript file size.
const fileofs kMaxJsFileLength = fileofs(2) * 1024 * 1024 * 1024;
}  // namespace

ScriptEngine::ScriptEngine() : _scopeInitCallback() {}

ScriptEngine::~ScriptEngine() {}

Scope::Scope()
    : _localDBName(""), _loadedVersion(0), _numTimesUsed(0), _lastRetIsNativeCode(false) {}

Scope::~Scope() {}

void Scope::append(BSONObjBuilder& builder, const char* fieldName, const char* scopeName) {
    int t = type(scopeName);
    switch (t) {
        case Object:
            builder.append(fieldName, getObject(scopeName));
            break;
        case Array:
            builder.appendArray(fieldName, getObject(scopeName));
            break;
        case NumberDouble:
            builder.append(fieldName, getNumber(scopeName));
            break;
        case NumberInt:
            builder.append(fieldName, getNumberInt(scopeName));
            break;
        case NumberLong:
            builder.append(fieldName, getNumberLongLong(scopeName));
            break;
        case NumberDecimal:
            builder.append(fieldName, getNumberDecimal(scopeName));
            break;
        case String:
            builder.append(fieldName, getString(scopeName));
            break;
        case Bool:
            builder.appendBool(fieldName, getBoolean(scopeName));
            break;
        case jstNULL:
        case Undefined:
            builder.appendNull(fieldName);
            break;
        case Date:
            builder.appendDate(fieldName,
                               Date_t::fromMillisSinceEpoch(getNumberLongLong(scopeName)));
            break;
        case Code:
            builder.appendCode(fieldName, getString(scopeName));
            break;
        default:
            uassert(10206, str::stream() << "can't append type from: " << t, 0);
    }
}

int Scope::invoke(const char* code, const BSONObj* args, const BSONObj* recv, int timeoutMs) {
    ScriptingFunction func = createFunction(code);
    uassert(10207, "compile failed", func);
    return invoke(func, args, recv, timeoutMs);
}

bool Scope::execFile(const string& filename, bool printResult, bool reportError, int timeoutMs) {
#ifdef _WIN32
    boost::filesystem::path p(toWideString(filename.c_str()));
#else
    boost::filesystem::path p(filename);
#endif
    if (!exists(p)) {
        error() << "file [" << filename << "] doesn't exist" << endl;
        return false;
    }

    // iterate directories and recurse using all *.js files in the directory
    if (boost::filesystem::is_directory(p)) {
        boost::filesystem::directory_iterator end;
        bool empty = true;

        for (boost::filesystem::directory_iterator it(p); it != end; it++) {
            empty = false;
            boost::filesystem::path sub(*it);
            if (!str::endsWith(sub.string().c_str(), ".js"))
                continue;
            if (!execFile(sub.string(), printResult, reportError, timeoutMs))
                return false;
        }

        if (empty) {
            error() << "directory [" << filename << "] doesn't have any *.js files" << endl;
            return false;
        }

        return true;
    }

    File f;
    f.open(filename.c_str(), true);

    if (!f.is_open() || f.bad())
        return false;

    fileofs fo = f.len();
    if (fo > kMaxJsFileLength) {
        warning() << "attempted to execute javascript file larger than 2GB" << endl;
        return false;
    }
    unsigned len = static_cast<unsigned>(fo);
    std::unique_ptr<char[]> data(new char[len + 1]);
    data[len] = 0;
    f.read(0, data.get(), len);

    int offset = 0;
    if (data[0] == '#' && data[1] == '!') {
        const char* newline = strchr(data.get(), '\n');
        if (!newline)
            return true;  // file of just shebang treated same as empty file
        offset = newline - data.get();
    }

    StringData code(data.get() + offset, len - offset);
    return exec(code, filename, printResult, reportError, false, timeoutMs);
}

class Scope::StoredFuncModLogOpHandler : public RecoveryUnit::Change {
public:
    void commit() {
        _lastVersion.fetchAndAdd(1);
    }
    void rollback() {}
};

void Scope::storedFuncMod(OperationContext* txn) {
    txn->recoveryUnit()->registerChange(new StoredFuncModLogOpHandler());
}

void Scope::validateObjectIdString(const string& str) {
    uassert(10448, "invalid object id: length", str.size() == 24);
    for (size_t i = 0; i < str.size(); i++)
        uassert(10430, "invalid object id: not hex", std::isxdigit(str.at(i)));
}

void Scope::loadStored(OperationContext* txn, bool ignoreNotConnected) {
    if (_localDBName.size() == 0) {
        if (ignoreNotConnected)
            return;
        uassert(10208, "need to have locallyConnected already", _localDBName.size());
    }

    int64_t lastVersion = _lastVersion.load();
    if (_loadedVersion == lastVersion)
        return;

    _loadedVersion = lastVersion;
    string coll = _localDBName + ".system.js";

    auto directDBClient = DBDirectClientFactory::get(txn).create(txn);

    unique_ptr<DBClientCursor> c =
        directDBClient->query(coll, Query(), 0, 0, NULL, QueryOption_SlaveOk, 0);
    massert(16669, "unable to get db client cursor from query", c.get());

    set<string> thisTime;
    while (c->more()) {
        BSONObj o = c->nextSafe().getOwned();
        BSONElement n = o["_id"];
        BSONElement v = o["value"];

        uassert(10209, str::stream() << "name has to be a string: " << n, n.type() == String);
        uassert(10210, "value has to be set", v.type() != EOO);

        try {
            setElement(n.valuestr(), v, o);
            thisTime.insert(n.valuestr());
            _storedNames.insert(n.valuestr());
        } catch (const DBException& setElemEx) {
            if (setElemEx.getCode() == ErrorCodes::Interrupted) {
                throw;
            }

            error() << "unable to load stored JavaScript function " << n.valuestr()
                    << "(): " << setElemEx.what() << endl;
        }
    }

    // remove things from scope that were removed from the system.js collection
    for (set<string>::iterator i = _storedNames.begin(); i != _storedNames.end();) {
        if (thisTime.count(*i) == 0) {
            string toDelete = str::stream() << "delete " << *i;
            _storedNames.erase(i++);
            execSetup(toDelete, "clean up scope");
        } else {
            ++i;
        }
    }
}

ScriptingFunction Scope::createFunction(const char* code) {
    if (code[0] == '/' && code[1] == '*') {
        code += 2;
        while (code[0] && code[1]) {
            if (code[0] == '*' && code[1] == '/') {
                code += 2;
                break;
            }
            code++;
        }
    }

    FunctionCacheMap::iterator i = _cachedFunctions.find(code);
    if (i != _cachedFunctions.end())
        return i->second;
    // NB: we calculate the function number for v8 so the cache can be utilized to
    //     lookup the source on an exception, but SpiderMonkey uses the value
    //     returned by JS_CompileFunction.
    ScriptingFunction defaultFunctionNumber = getFunctionCache().size() + 1;
    ScriptingFunction actualFunctionNumber = _createFunction(code, defaultFunctionNumber);
    _cachedFunctions[code] = actualFunctionNumber;
    return actualFunctionNumber;
}

namespace JSFiles {
extern const JSFile collection;
extern const JSFile crud_api;
extern const JSFile db;
extern const JSFile explain_query;
extern const JSFile explainable;
extern const JSFile mongo;
extern const JSFile mr;
extern const JSFile query;
extern const JSFile upgrade_check;
extern const JSFile utils;
extern const JSFile utils_sh;
extern const JSFile utils_auth;
extern const JSFile bulk_api;
extern const JSFile error_codes;
}

void Scope::execCoreFiles() {
    execSetup(JSFiles::utils);
    execSetup(JSFiles::utils_sh);
    execSetup(JSFiles::utils_auth);
    execSetup(JSFiles::db);
    execSetup(JSFiles::mongo);
    execSetup(JSFiles::mr);
    execSetup(JSFiles::query);
    execSetup(JSFiles::bulk_api);
    execSetup(JSFiles::error_codes);
    execSetup(JSFiles::collection);
    execSetup(JSFiles::crud_api);
    execSetup(JSFiles::explain_query);
    execSetup(JSFiles::explainable);
    execSetup(JSFiles::upgrade_check);
}

namespace {
class ScopeCache {
public:
    void release(const string& poolName, const std::shared_ptr<Scope>& scope) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (scope->hasOutOfMemoryException()) {
            // make some room
            log() << "Clearing all idle JS contexts due to out of memory" << endl;
            _pools.clear();
            return;
        }

        if (scope->getTimesUsed() > kMaxScopeReuse)
            return;  // used too many times to save

        if (!scope->getError().empty())
            return;  // not saving errored scopes

        if (_pools.size() >= kMaxPoolSize) {
            // prefer to keep recently-used scopes
            _pools.pop_back();
        }

        scope->reset();
        ScopeAndPool toStore = {scope, poolName};
        _pools.push_front(toStore);
    }

    std::shared_ptr<Scope> tryAcquire(OperationContext* txn, const string& poolName) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        for (Pools::iterator it = _pools.begin(); it != _pools.end(); ++it) {
            if (it->poolName == poolName) {
                std::shared_ptr<Scope> scope = it->scope;
                _pools.erase(it);
                scope->incTimesUsed();
                scope->reset();
                scope->registerOperation(txn);
                return scope;
            }
        }

        return std::shared_ptr<Scope>();
    }

    void clear() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        _pools.clear();
    }

private:
    struct ScopeAndPool {
        std::shared_ptr<Scope> scope;
        string poolName;
    };

    // Note: if these numbers change, reconsider choice of datastructure for _pools
    static const unsigned kMaxPoolSize = 10;
    static const int kMaxScopeReuse = 10;

    typedef std::deque<ScopeAndPool> Pools;  // More-recently used Scopes are kept at the front.
    Pools _pools;                            // protected by _mutex
    stdx::mutex _mutex;
};

ScopeCache scopeCache;
}  // anonymous namespace

void ScriptEngine::dropScopeCache() {
    scopeCache.clear();
}

class PooledScope : public Scope {
public:
    PooledScope(const std::string& pool, const std::shared_ptr<Scope>& real)
        : _pool(pool), _real(real) {}

    virtual ~PooledScope() {
        scopeCache.release(_pool, _real);
    }

    // wrappers for the derived (_real) scope
    void reset() {
        _real->reset();
    }
    void registerOperation(OperationContext* txn) {
        _real->registerOperation(txn);
    }
    void unregisterOperation() {
        _real->unregisterOperation();
    }
    void init(const BSONObj* data) {
        _real->init(data);
    }
    void localConnectForDbEval(OperationContext* txn, const char* dbName) {
        invariant(!"localConnectForDbEval should only be called from dbEval");
    }
    void setLocalDB(const string& dbName) {
        _real->setLocalDB(dbName);
    }
    void loadStored(OperationContext* txn, bool ignoreNotConnected = false) {
        _real->loadStored(txn, ignoreNotConnected);
    }
    void externalSetup() {
        _real->externalSetup();
    }
    void gc() {
        _real->gc();
    }
    void advanceGeneration() {
        _real->advanceGeneration();
    }
    bool isKillPending() const {
        return _real->isKillPending();
    }
    int type(const char* field) {
        return _real->type(field);
    }
    string getError() {
        return _real->getError();
    }
    bool hasOutOfMemoryException() {
        return _real->hasOutOfMemoryException();
    }
    void rename(const char* from, const char* to) {
        _real->rename(from, to);
    }
    double getNumber(const char* field) {
        return _real->getNumber(field);
    }
    int getNumberInt(const char* field) {
        return _real->getNumberInt(field);
    }
    long long getNumberLongLong(const char* field) {
        return _real->getNumberLongLong(field);
    }
    Decimal128 getNumberDecimal(const char* field) {
        return _real->getNumberDecimal(field);
    }
    string getString(const char* field) {
        return _real->getString(field);
    }
    bool getBoolean(const char* field) {
        return _real->getBoolean(field);
    }
    BSONObj getObject(const char* field) {
        return _real->getObject(field);
    }
    void setNumber(const char* field, double val) {
        _real->setNumber(field, val);
    }
    void setString(const char* field, StringData val) {
        _real->setString(field, val);
    }
    void setElement(const char* field, const BSONElement& val, const BSONObj& parent) {
        _real->setElement(field, val, parent);
    }
    void setObject(const char* field, const BSONObj& obj, bool readOnly = true) {
        _real->setObject(field, obj, readOnly);
    }
    bool isLastRetNativeCode() {
        return _real->isLastRetNativeCode();
    }

    void setBoolean(const char* field, bool val) {
        _real->setBoolean(field, val);
    }
    void setFunction(const char* field, const char* code) {
        _real->setFunction(field, code);
    }
    ScriptingFunction createFunction(const char* code) {
        return _real->createFunction(code);
    }
    int invoke(ScriptingFunction func,
               const BSONObj* args,
               const BSONObj* recv,
               int timeoutMs,
               bool ignoreReturn,
               bool readOnlyArgs,
               bool readOnlyRecv) {
        return _real->invoke(func, args, recv, timeoutMs, ignoreReturn, readOnlyArgs, readOnlyRecv);
    }
    bool exec(StringData code,
              const string& name,
              bool printResult,
              bool reportError,
              bool assertOnError,
              int timeoutMs = 0) {
        return _real->exec(code, name, printResult, reportError, assertOnError, timeoutMs);
    }
    bool execFile(const string& filename, bool printResult, bool reportError, int timeoutMs = 0) {
        return _real->execFile(filename, printResult, reportError, timeoutMs);
    }
    void injectNative(const char* field, NativeFunction func, void* data) {
        _real->injectNative(field, func, data);
    }
    void append(BSONObjBuilder& builder, const char* fieldName, const char* scopeName) {
        _real->append(builder, fieldName, scopeName);
    }

protected:
    FunctionCacheMap& getFunctionCache() {
        return _real->getFunctionCache();
    }

    ScriptingFunction _createFunction(const char* code, ScriptingFunction functionNumber = 0) {
        return _real->_createFunction(code, functionNumber);
    }

private:
    string _pool;
    std::shared_ptr<Scope> _real;
};

/** Get a scope from the pool of scopes matching the supplied pool name */
unique_ptr<Scope> ScriptEngine::getPooledScope(OperationContext* txn,
                                               const string& db,
                                               const string& scopeType) {
    const string fullPoolName = db + scopeType;
    std::shared_ptr<Scope> s = scopeCache.tryAcquire(txn, fullPoolName);
    if (!s) {
        s.reset(newScope());
        s->registerOperation(txn);
    }

    unique_ptr<Scope> p;
    p.reset(new PooledScope(fullPoolName, s));
    p->setLocalDB(db);
    p->loadStored(txn, true);
    return p;
}

void (*ScriptEngine::_connectCallback)(DBClientWithCommands&) = 0;
ScriptEngine* globalScriptEngine = 0;

bool hasJSReturn(const string& code) {
    size_t x = code.find("return");
    if (x == string::npos)
        return false;

    int quoteCount = 0;
    int singleQuoteCount = 0;
    for (size_t i = 0; i < x; i++) {
        if (code[i] == '"') {
            quoteCount++;
        } else if (code[i] == '\'') {
            singleQuoteCount++;
        }
    }
    // if we are in either single quotes or double quotes return false
    if (quoteCount % 2 != 0 || singleQuoteCount % 2 != 0) {
        return false;
    }

    // return is at start OR preceded by space
    // AND return is not followed by digit or letter
    return (x == 0 || isspace(code[x - 1])) && !(isalpha(code[x + 6]) || isdigit(code[x + 6]));
}

const char* jsSkipWhiteSpace(const char* raw) {
    while (raw[0]) {
        while (isspace(*raw)) {
            ++raw;
        }
        if (raw[0] != '/' || raw[1] != '/')
            break;
        while (raw[0] && raw[0] != '\n')
            raw++;
    }
    return raw;
}
}
