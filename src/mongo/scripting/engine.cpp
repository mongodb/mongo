/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <algorithm>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>

#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/ctype.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/file.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

AtomicWord<long long> Scope::_lastVersion(1);


namespace {

MONGO_FAIL_POINT_DEFINE(mr_killop_test_fp);
// 2 GB is the largest support Javascript file size.
const fileofs kMaxJsFileLength = fileofs(2) * 1024 * 1024 * 1024;

const ServiceContext::Decoration<std::unique_ptr<ScriptEngine>> forService =
    ServiceContext::declareDecoration<std::unique_ptr<ScriptEngine>>();
static std::unique_ptr<ScriptEngine> globalScriptEngine;

}  // namespace

ScriptEngine::ScriptEngine() : _scopeInitCallback() {}

Scope::Scope()
    : _localDBName(DatabaseName::kEmpty),
      _loadedVersion(0),
      _createTime(Date_t::now()),
      _lastRetIsNativeCode(false) {}

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
        case jstOID:
            builder.append(fieldName, getOID(scopeName));
            break;
        case BinData:
            getBinData(scopeName, [&fieldName, &builder](const BSONBinData& binData) {
                builder.append(fieldName, binData);
            });
            break;
        case bsonTimestamp:
            builder.append(fieldName, getTimestamp(scopeName));
            break;
        case MinKey:
            builder.appendMinKey(fieldName);
            break;
        case MaxKey:
            builder.appendMaxKey(fieldName);
            break;
        case RegEx: {
            auto regEx = getRegEx(scopeName);
            builder.append(fieldName, BSONRegEx{regEx.pattern, regEx.flags});
            break;
        }
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
        LOGV2_ERROR(22779, "file [{filename}] doesn't exist", "filename"_attr = filename);
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
            LOGV2_ERROR(22780,
                        "directory [{filename}] doesn't have any *.js files",
                        "filename"_attr = filename);
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
        LOGV2_WARNING(22778, "attempted to execute javascript file larger than 2GB");
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

void Scope::storedFuncMod(OperationContext* opCtx) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [](OperationContext*, boost::optional<Timestamp>) { _lastVersion.fetchAndAdd(1); });
}

void Scope::validateObjectIdString(const string& str) {
    uassert(10448, "invalid object id: length", str.size() == 24);
    auto isAllHex = [](StringData s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return ctype::isXdigit(c); });
    };
    uassert(10430, "invalid object id: not hex", isAllHex(str));
}

void Scope::loadStored(OperationContext* opCtx, bool ignoreNotConnected) {
    if (_localDBName.isEmpty()) {
        if (ignoreNotConnected)
            return;
        uassert(10208, "need to have locallyConnected already", _localDBName.size());
    }

    int64_t lastVersion = _lastVersion.load();
    if (_loadedVersion == lastVersion)
        return;

    const auto collNss = NamespaceStringUtil::deserialize(_localDBName, "system.js");

    auto directDBClient = DBDirectClientFactory::get(opCtx).create(opCtx);

    std::unique_ptr<DBClientCursor> c = directDBClient->find(
        FindCommandRequest{collNss}, ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
    massert(16669, "unable to get db client cursor from query", c.get());

    set<string> thisTime;
    while (c->more()) {
        BSONObj o = c->nextSafe().getOwned();
        BSONElement n = o["_id"];
        BSONElement v = o["value"];

        uassert(
            10209, str::stream() << "name has to be a string: " << n, n.type() == BSONType::String);
        uassert(10210, "value has to be set", v.type() != BSONType::EOO);

        uassert(4546000,
                str::stream() << "BSON type 'CodeWithScope' not supported in system.js scripts. As "
                                 "an alternative use 'Code'. Script _id value: '"
                              << n.String() << "'",
                v.type() != BSONType::CodeWScope);

        if (MONGO_unlikely(mr_killop_test_fp.shouldFail())) {
            LOGV2(5062200, "Pausing mr_killop_test_fp for system.js entry", "entryName"_attr = n);

            /* This thread sleep makes the interrupts in the test come in at a time
             *  where the js misses the interrupt and throw an exception instead of
             *  being interrupted
             */
            stdx::this_thread::sleep_for(stdx::chrono::seconds(1));
        }

        try {
            setElement(n.valueStringDataSafe().rawData(), v, o);
            thisTime.insert(n.str());
            _storedNames.insert(n.str());
        } catch (const DBException& setElemEx) {
            if (setElemEx.code() == ErrorCodes::Interrupted) {
                throw;
            }

            LOGV2_ERROR(22781,
                        "unable to load stored JavaScript function {n_valuestr}(): {setElemEx}",
                        "n_valuestr"_attr = n.valueStringDataSafe(),
                        "setElemEx"_attr = redact(setElemEx));
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

    // Only update _loadedVersion if loading system.js completed successfully.
    // If any one operation failed or was interrupted we will start over next time.
    _loadedVersion = lastVersion;
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

    // Get a function number, so the cache can be utilized to lookup the source on an exception
    ScriptingFunction functionNumber = _createFunction(code);
    _cachedFunctions[code] = functionNumber;
    return functionNumber;
}

namespace JSFiles {
extern const JSFile collection;
extern const JSFile check_log;
extern const JSFile crud_api;
extern const JSFile db;
extern const JSFile explain_query;
extern const JSFile explainable;
extern const JSFile mongo;
extern const JSFile session;
extern const JSFile query;
extern const JSFile utils;
extern const JSFile utils_sh;
extern const JSFile utils_auth;
extern const JSFile bulk_api;
extern const JSFile error_codes;
}  // namespace JSFiles

void Scope::execCoreFiles() {
    execSetup(JSFiles::utils);
    execSetup(JSFiles::utils_sh);
    execSetup(JSFiles::utils_auth);
    execSetup(JSFiles::db);
    execSetup(JSFiles::mongo);
    execSetup(JSFiles::session);
    execSetup(JSFiles::query);
    execSetup(JSFiles::bulk_api);
    execSetup(JSFiles::error_codes);
    execSetup(JSFiles::check_log);
    execSetup(JSFiles::collection);
    execSetup(JSFiles::crud_api);
    execSetup(JSFiles::explain_query);
    execSetup(JSFiles::explainable);
}

namespace {

class ScopeCache {
public:
    using PoolName = std::tuple<DatabaseName, string>;
    void release(const PoolName& poolName, const std::shared_ptr<Scope>& scope) {
        stdx::lock_guard<Latch> lk(_mutex);

        if (scope->hasOutOfMemoryException()) {
            // make some room
            LOGV2_INFO(22777, "Clearing all idle JS contexts due to out of memory");
            _pools.clear();
            return;
        }

        if (Date_t::now() - scope->getCreateTime() > kMaxScopeReuseTime) {
            return;  // too old to save
        }

        if (!scope->getError().empty()) {
            return;  // not saving errored scopes
        }

        if (_pools.size() >= kMaxPoolSize) {
            // prefer to keep recently-used scopes
            _pools.pop_back();
        }

        scope->reset();
        ScopeAndPool toStore = {scope, poolName};
        _pools.push_front(toStore);
    }

    std::shared_ptr<Scope> tryAcquire(OperationContext* opCtx, const PoolName& poolName) {
        stdx::lock_guard<Latch> lk(_mutex);

        for (Pools::iterator it = _pools.begin(); it != _pools.end(); ++it) {
            if (it->poolName == poolName) {
                std::shared_ptr<Scope> scope = it->scope;
                _pools.erase(it);
                scope->reset();
                scope->registerOperation(opCtx);
                return scope;
            }
        }

        return std::shared_ptr<Scope>();
    }

    void clear() {
        stdx::lock_guard<Latch> lk(_mutex);

        _pools.clear();
    }

private:
    struct ScopeAndPool {
        std::shared_ptr<Scope> scope;
        std::tuple<DatabaseName, string /*scopeType*/> poolName;
    };

    // Note: if these numbers change, reconsider choice of datastructure for _pools
    static const unsigned kMaxPoolSize = 10;
    constexpr static inline Seconds kMaxScopeReuseTime = Seconds(10);

    typedef std::deque<ScopeAndPool> Pools;  // More-recently used Scopes are kept at the front.
    Pools _pools;                            // protected by _mutex
    Mutex _mutex = MONGO_MAKE_LATCH("ScopeCache::_mutex");
};

ScopeCache scopeCache;
}  // anonymous namespace

void ScriptEngine::dropScopeCache() {
    scopeCache.clear();
}

class PooledScope : public Scope {
public:
    PooledScope(const ScopeCache::PoolName& pool, const std::shared_ptr<Scope>& real)
        : _pool(pool), _real(real) {}

    virtual ~PooledScope() {
        // SERVER-53671: Sometimes, ScopeCache::release() will generate an 'InterruptedAtShutdown'
        // exception. We catch and ignore such exceptions here to prevent them from crashing the
        // server while it is shutting down.
        try {
            scopeCache.release(_pool, _real);
        } catch (const ExceptionFor<ErrorCodes::InterruptedAtShutdown>&) {
            LOGV2(5367100, "Interrupted at shutdown during ~PooledScope()");
        }
    }

    // wrappers for the derived (_real) scope
    void reset() {
        _real->reset();
    }
    void registerOperation(OperationContext* opCtx) {
        _real->registerOperation(opCtx);
    }
    void unregisterOperation() {
        _real->unregisterOperation();
    }
    void init(const BSONObj* data) {
        _real->init(data);
    }
    void setLocalDB(const DatabaseName& dbName) {
        _real->setLocalDB(dbName);
    }
    void loadStored(OperationContext* opCtx, bool ignoreNotConnected = false) {
        _real->loadStored(opCtx, ignoreNotConnected);
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
    void requireOwnedObjects() override {
        _real->requireOwnedObjects();
    }
    void kill() {
        _real->kill();
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
    OID getOID(const char* field) {
        return _real->getOID(field);
    };
    void getBinData(const char* field, std::function<void(const BSONBinData&)> withBinData) {
        _real->getBinData(field, std::move(withBinData));
    }
    Timestamp getTimestamp(const char* field) {
        return _real->getTimestamp(field);
    };
    JSRegEx getRegEx(const char* field) {
        return _real->getRegEx(field);
    };
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
    ScriptingFunction _createFunction(const char* code) {
        return _real->_createFunction(code);
    }

private:
    ScopeCache::PoolName _pool;
    std::shared_ptr<Scope> _real;
};

/** Get a scope from the pool of scopes matching the supplied pool name */
unique_ptr<Scope> ScriptEngine::getPooledScope(OperationContext* opCtx,
                                               const DatabaseName& db,
                                               const string& scopeType) {
    const auto fullPoolName = std::make_tuple(db, scopeType);
    std::shared_ptr<Scope> s = scopeCache.tryAcquire(opCtx, fullPoolName);
    if (!s) {
        s.reset(newScope());
        s->registerOperation(opCtx);
    }

    unique_ptr<Scope> p;
    p.reset(new PooledScope(fullPoolName, s));
    p->setLocalDB(db);
    p->loadStored(opCtx, true);
    return p;
}

void (*ScriptEngine::_connectCallback)(DBClientBase&, StringData) = nullptr;

ScriptEngine* getGlobalScriptEngine() {
    if (hasGlobalServiceContext())
        return forService(getGlobalServiceContext()).get();
    else
        return globalScriptEngine.get();
}

void setGlobalScriptEngine(ScriptEngine* impl) {
    if (hasGlobalServiceContext())
        forService(getGlobalServiceContext()).reset(impl);
    else
        globalScriptEngine.reset(impl);
}

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
    return (x == 0 || ctype::isSpace(code[x - 1])) &&
        !(ctype::isAlpha(code[x + 6]) || ctype::isDigit(code[x + 6]));
}

const char* jsSkipWhiteSpace(const char* raw) {
    while (raw[0]) {
        while (ctype::isSpace(*raw)) {
            ++raw;
        }
        if (raw[0] != '/' || raw[1] != '/')
            break;
        while (raw[0] && raw[0] != '\n')
            raw++;
    }
    return raw;
}
}  // namespace mongo
