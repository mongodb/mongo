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

#include "mongo/platform/basic.h"

#include <cstddef>
#include <curl/curl.h>
#include <curl/easy.h>
#include <memory>
#include <string>

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/alarm.h"
#include "mongo/util/alarm_runner_background_thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/net/http_client_options.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/strong_weak_finish_line.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {

namespace {
using namespace executor;

/**
 * Curl Protocol configuration supported by HttpClient
 */
enum class Protocols {
    // Allow either http or https, unsafe
    kHttpOrHttps,

    // Allow https only
    kHttpsOnly,
};

// Connection pool talk in terms of Mongo's SSL configuration.
// These functions provide a way to map back and forth between them.
transport::ConnectSSLMode mapProtocolToSSLMode(Protocols protocol) {
    return (protocol == Protocols::kHttpsOnly) ? transport::kEnableSSL : transport::kDisableSSL;
}

Protocols mapSSLModeToProtocol(transport::ConnectSSLMode sslMode) {
    return (sslMode == transport::kEnableSSL) ? Protocols::kHttpsOnly : Protocols::kHttpOrHttps;
}

class CurlLibraryManager {
public:
    // No copying and no moving because we give libcurl the address of our members.
    // In practice, we'll never want to copy/move this instance anyway,
    // but if that ever changes, we can write trivial implementations to deal with it.
    CurlLibraryManager(const CurlLibraryManager&) = delete;
    CurlLibraryManager& operator=(const CurlLibraryManager&) = delete;
    CurlLibraryManager(CurlLibraryManager&&) = delete;
    CurlLibraryManager& operator=(CurlLibraryManager&&) = delete;

    CurlLibraryManager() = default;
    ~CurlLibraryManager() {
        // Ordering matters: curl_global_cleanup() must happen last.
        if (_initialized) {
            curl_global_cleanup();
        }
    }

    Status initialize() {
        if (_initialized) {
            return {ErrorCodes::AlreadyInitialized, "CurlLibraryManager already initialized."};
        }

        CURLcode ret = curl_global_init(CURL_GLOBAL_ALL);
        if (ret != CURLE_OK) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Failed to initialize CURL: " << static_cast<int64_t>(ret)};
        }

        curl_version_info_data* version_data = curl_version_info(CURLVERSION_NOW);
        if (!(version_data->features & CURL_VERSION_SSL)) {
            return {ErrorCodes::InternalError, "Curl lacks SSL support, cannot continue"};
        }

        _initialized = true;
        return Status::OK();
    }

    bool isInitialized() const {
        return _initialized;
    }

private:
    bool _initialized = false;

} curlLibraryManager;

/**
 * Receives data from the remote side.
 */
size_t WriteMemoryCallback(void* ptr, size_t size, size_t nmemb, void* data) {
    const size_t realsize = size * nmemb;

    auto* mem = reinterpret_cast<DataBuilder*>(data);
    if (!mem->writeAndAdvance(ConstDataRange(reinterpret_cast<const char*>(ptr),
                                             reinterpret_cast<const char*>(ptr) + realsize))
             .isOK()) {
        // Cause curl to generate a CURLE_WRITE_ERROR by returning a different number than how much
        // data there was to write.
        return 0;
    }

    return realsize;
}

/**
 * Sends data to the remote side
 */
size_t ReadMemoryCallback(char* buffer, size_t size, size_t nitems, void* instream) {

    auto* bufReader = reinterpret_cast<BufReader*>(instream);

    size_t ret = 0;

    if (bufReader->remaining() > 0) {
        size_t readSize =
            std::min(size * nitems, static_cast<unsigned long>(bufReader->remaining()));
        auto buf = bufReader->readBytes(readSize);
        memcpy(buffer, buf.rawData(), readSize);
        ret = readSize;
    }

    return ret;
}

/**
 * Seek into for data to the remote side
 */
size_t SeekMemoryCallback(void* clientp, curl_off_t offset, int origin) {

    // Curl will call this in readrewind but only to reset the stream to the beginning
    // In other protocols (like FTP, SSH) or HTTP resumption they may ask for partial buffers which
    // we do not support.
    if (offset != 0 || origin != SEEK_SET) {
        return CURL_SEEKFUNC_CANTSEEK;
    }

    auto* bufReader = reinterpret_cast<BufReader*>(clientp);

    bufReader->rewindToStart();

    return CURL_SEEKFUNC_OK;
}

struct CurlEasyCleanup {
    void operator()(CURL* handle) {
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }
};
using CurlEasyHandle = std::unique_ptr<CURL, CurlEasyCleanup>;

struct CurlSlistFreeAll {
    void operator()(curl_slist* list) {
        if (list) {
            curl_slist_free_all(list);
        }
    }
};
using CurlSlist = std::unique_ptr<curl_slist, CurlSlistFreeAll>;


long longSeconds(Seconds tm) {
    return static_cast<long>(durationCount<Seconds>(tm));
}


StringData enumToString(curl_infotype type) {
    switch (type) {
        case CURLINFO_TEXT:
            return "TEXT"_sd;
        case CURLINFO_HEADER_IN:
            return "HEADER_IN"_sd;
        case CURLINFO_HEADER_OUT:
            return "HEADER_OUT"_sd;
        case CURLINFO_DATA_IN:
            return "DATA_IN"_sd;
        case CURLINFO_DATA_OUT:
            return "DATA_OUT"_sd;
        case CURLINFO_SSL_DATA_IN:
            return "SSL_DATA_IN"_sd;
        case CURLINFO_SSL_DATA_OUT:
            return "SSL_DATA_OUT"_sd;
        default:
            return "unknown"_sd;
    }
}

int curlDebugCallback(CURL* handle, curl_infotype type, char* data, size_t size, void* clientp) {
    switch (type) {
        case CURLINFO_TEXT:
        case CURLINFO_HEADER_IN:
        case CURLINFO_HEADER_OUT:
        case CURLINFO_DATA_IN:
        case CURLINFO_DATA_OUT:
            LOGV2_DEBUG(7661901,
                        1,
                        "Curl",
                        "type"_attr = enumToString(type),
                        "message"_attr = StringData(data, size));
            [[fallthrough]];

        default:
            return 0;
    }
}

CurlEasyHandle createCurlEasyHandle(Protocols protocol) {
    CurlEasyHandle handle(curl_easy_init());
    uassert(ErrorCodes::InternalError, "Curl initialization failed", handle);

    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, longSeconds(kConnectionTimeout));
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0);
    curl_easy_setopt(handle.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#ifdef CURLOPT_TCP_KEEPALIVE
    curl_easy_setopt(handle.get(), CURLOPT_TCP_KEEPALIVE, 1);
#endif
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, longSeconds(kTotalRequestTimeout));

#if LIBCURL_VERSION_NUM > 0x072200
    // Requires >= 7.34.0
    curl_easy_setopt(handle.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#endif


    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, WriteMemoryCallback);

    if (protocol == Protocols::kHttpOrHttps) {
        curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
    } else {
        curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    }

    // TODO: CURLOPT_EXPECT_100_TIMEOUT_MS?
    if (httpClientOptions.verboseLogging.loadRelaxed()) {
        curl_easy_setopt(handle.get(), CURLOPT_VERBOSE, 1);
        curl_easy_setopt(handle.get(), CURLOPT_DEBUGFUNCTION, curlDebugCallback);
    }

    return handle;
}

ConnectionPool::Options makePoolOptions(Seconds timeout) {
    ConnectionPool::Options opts;
    opts.refreshTimeout = timeout;
    opts.refreshRequirement = Seconds(60);
    opts.hostTimeout = Seconds(120);
    return opts;
}

/*
 * This implements the timer interface for the ConnectionPool.
 * Timers will be expired in order on a single background thread.
 */
class CurlHandleTimer : public ConnectionPool::TimerInterface {
public:
    explicit CurlHandleTimer(ClockSource* clockSource, std::shared_ptr<AlarmScheduler> scheduler)
        : _clockSource(clockSource), _scheduler(std::move(scheduler)), _handle(nullptr) {}

    virtual ~CurlHandleTimer() {
        if (_handle) {
            _handle->cancel().ignore();
        }
    }

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) final {
        auto res = _scheduler->alarmFromNow(timeout);
        _handle = std::move(res.handle);

        std::move(res.future).getAsync([cb](Status status) {
            if (status == ErrorCodes::CallbackCanceled) {
                return;
            }

            fassert(5413901, status);
            cb();
        });
    }

    void cancelTimeout() final {
        auto handle = std::move(_handle);
        if (handle) {
            handle->cancel().ignore();
        }
    }

    Date_t now() final {
        return _clockSource->now();
    }

private:
    ClockSource* const _clockSource;
    std::shared_ptr<AlarmScheduler> _scheduler;
    AlarmScheduler::SharedHandle _handle;
};

/**
 * Type factory that manages the curl connection pool
 */
class CurlHandleTypeFactory : public executor::ConnectionPool::DependentTypeFactoryInterface {
public:
    CurlHandleTypeFactory()
        : _clockSource(SystemClockSource::get()),
          _executor(std::make_shared<ThreadPool>(_makeThreadPoolOptions())),
          _timerScheduler(std::make_shared<AlarmSchedulerPrecise>(_clockSource)),
          _timerRunner({_timerScheduler}) {}

    std::shared_ptr<ConnectionPool::ConnectionInterface> makeConnection(const HostAndPort&,
                                                                        transport::ConnectSSLMode,
                                                                        size_t generation) final;

    std::shared_ptr<ConnectionPool::TimerInterface> makeTimer() final {
        _start();
        return std::make_shared<CurlHandleTimer>(_clockSource, _timerScheduler);
    }

    const std::shared_ptr<OutOfLineExecutor>& getExecutor() final {
        return _executor;
    }

    Date_t now() final {
        return _clockSource->now();
    }

    void shutdown() final {
        if (!_running) {
            return;
        }
        _timerRunner.shutdown();

        auto pool = checked_pointer_cast<ThreadPool>(_executor);
        pool->shutdown();
        pool->join();
    }

private:
    void _start() {
        if (_running)
            return;
        _timerRunner.start();

        auto pool = checked_pointer_cast<ThreadPool>(_executor);
        pool->startup();

        _running = true;
    }

    static inline ThreadPool::Options _makeThreadPoolOptions() {
        ThreadPool::Options opts;
        opts.poolName = "CurlConnPool";
        opts.maxThreads = ThreadPool::Options::kUnlimited;
        opts.maxIdleThreadAge = Seconds{5};

        return opts;
    }

private:
    ClockSource* const _clockSource;
    std::shared_ptr<OutOfLineExecutor> _executor;
    std::shared_ptr<AlarmScheduler> _timerScheduler;
    bool _running = false;
    AlarmRunnerBackgroundThread _timerRunner;
};


/**
 * Curl handle that is managed by a connection pool
 *
 * The connection pool does not manage actual connections, just handles. Curl has automatica
 * reconnect logic if it gets disconnected. Also, HTTP connections are cheaper then MongoDB.
 */
class PooledCurlHandle : public ConnectionPool::ConnectionInterface,
                         public std::enable_shared_from_this<PooledCurlHandle> {
public:
    PooledCurlHandle(std::shared_ptr<OutOfLineExecutor> executor,
                     ClockSource* clockSource,
                     const std::shared_ptr<AlarmScheduler>& alarmScheduler,
                     const HostAndPort& host,
                     Protocols protocol,
                     size_t generation)
        : ConnectionInterface(generation),
          _executor(std::move(executor)),
          _alarmScheduler(alarmScheduler),
          _timer(clockSource, alarmScheduler),
          _target(host),
          _protocol(protocol) {}


    virtual ~PooledCurlHandle() = default;

    const HostAndPort& getHostAndPort() const final {
        return _target;
    }

    // This cannot block under any circumstances because the ConnectionPool is holding
    // a mutex while calling isHealthy(). Since we don't have a good way of knowing whether
    // the connection is healthy, just return true here.
    bool isHealthy() final {
        return true;
    }

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) final {
        _timer.setTimeout(timeout, cb);
    }

    void cancelTimeout() final {
        _timer.cancelTimeout();
    }

    Date_t now() final {
        return _timer.now();
    }

    transport::ConnectSSLMode getSslMode() const final {
        return mapProtocolToSSLMode(_protocol);
    }

    CURL* get() {
        return _handle.get();
    }

private:
    void setup(Milliseconds timeout, SetupCallback cb, std::string) final;

    void refresh(Milliseconds timeout, RefreshCallback cb) final;

private:
    std::shared_ptr<OutOfLineExecutor> _executor;
    std::shared_ptr<AlarmScheduler> _alarmScheduler;
    CurlHandleTimer _timer;
    HostAndPort _target;

    Protocols _protocol;
    CurlEasyHandle _handle;
};

void PooledCurlHandle::setup(Milliseconds timeout, SetupCallback cb, std::string) {
    auto anchor = shared_from_this();
    _executor->schedule([this, anchor, cb = std::move(cb)](auto execStatus) {
        if (!execStatus.isOK()) {
            cb(this, execStatus);
            return;
        }

        _handle = createCurlEasyHandle(_protocol);

        cb(this, Status::OK());
    });
}

void PooledCurlHandle::refresh(Milliseconds timeout, RefreshCallback cb) {
    auto anchor = shared_from_this();
    _executor->schedule([this, anchor, cb = std::move(cb)](auto execStatus) {
        if (!execStatus.isOK()) {
            cb(this, execStatus);
            return;
        }

        // Tell the connection pool that it was a success. Curl reconnects seamlessly behind the
        // scenes and there is no reliable way to test if the connection is still alive in a
        // connection agnostic way. HTTP verbs like HEAD are not uniformly supported.
        //
        // The connection pool simply needs to prune handles on a timer for us.
        indicateSuccess();
        indicateUsed();

        cb(this, Status::OK());
    });
}

std::shared_ptr<executor::ConnectionPool::ConnectionInterface>
CurlHandleTypeFactory::makeConnection(const HostAndPort& host,
                                      transport::ConnectSSLMode sslMode,
                                      size_t generation) {
    _start();

    return std::make_shared<PooledCurlHandle>(
        _executor, _clockSource, _timerScheduler, host, mapSSLModeToProtocol(sslMode), generation);
}

/**
 * Handle that manages connection pool semantics and returns handle to connection pool in
 * destructor.
 *
 * Caller must call indiciateSuccess if they want the handle to be reused.
 */
class CurlHandle {
public:
    CurlHandle(executor::ConnectionPool::ConnectionHandle handle, CURL* curlHandle)
        : _poolHandle(std::move(handle)), _handle(curlHandle) {}

    CurlHandle(CurlHandle&& other) = default;

    ~CurlHandle() {
        if (!_finished && _poolHandle.get() != nullptr) {
            _poolHandle->indicateFailure(
                Status(ErrorCodes::HostUnreachable, "unknown curl handle failure"));
        }
    }

    CURL* get() {
        return _handle;
    }

    void indicateSuccess() {
        _poolHandle->indicateSuccess();

        // Tell the connection pool that we used the connection otherwise the pool will be believe
        // the connection went idle since it is possible to checkout a connection and not actually
        // use it.
        _poolHandle->indicateUsed();

        _finished = true;
    }

    void indicateFailure(const Status& status) {
        if (_poolHandle.get() != nullptr) {
            _poolHandle->indicateFailure(status);
        }

        _finished = true;
    }

private:
    executor::ConnectionPool::ConnectionHandle _poolHandle;
    bool _finished = false;

    // Owned by _poolHandle
    CURL* _handle;
};

/**
 * Factory that returns curl handles managed in connection pool
 */
class CurlPool {
public:
    CurlPool()
        : _typeFactory(std::make_shared<CurlHandleTypeFactory>()),
          _pool(std::make_shared<executor::ConnectionPool>(
              _typeFactory, "Curl", makePoolOptions(Seconds(60)))) {}

    StatusWith<CurlHandle> get(HostAndPort server, Protocols protocol);

private:
    std::shared_ptr<CurlHandleTypeFactory> _typeFactory;
    std::shared_ptr<executor::ConnectionPool> _pool;
};

StatusWith<CurlHandle> CurlPool::get(HostAndPort server, Protocols protocol) {

    auto sslMode = mapProtocolToSSLMode(protocol);

    auto semi = _pool->get(server, sslMode, Seconds(60));

    StatusWith<executor::ConnectionPool::ConnectionHandle> swHandle = std::move(semi).getNoThrow();
    if (!swHandle.isOK()) {
        return swHandle.getStatus();
    }

    auto curlHandle = static_cast<PooledCurlHandle*>(swHandle.getValue().get())->get();

    return {CurlHandle(std::move(swHandle.getValue()), curlHandle)};
}

HostAndPort exactHostAndPortFromUrl(StringData url) {
    // Treat the URL as a host and port
    // URL: http(s)?://(host):(port)/...
    //
    constexpr StringData slashes = "//"_sd;
    auto slashesIndex = url.find(slashes);
    uassert(5413902, str::stream() << "//, URL: " << url, slashesIndex != std::string::npos);

    url = url.substr(slashesIndex + slashes.size());
    if (url.find('/') != std::string::npos) {
        url = url.substr(0, url.find("/"));
    }

    auto hp = HostAndPort(url);
    if (!hp.hasPort()) {
        if (url.startsWith("http://"_sd)) {
            return HostAndPort(hp.host(), 80);
        }

        return HostAndPort(hp.host(), 443);
    }

    return hp;
}

/**
 * The connection pool requires the ability to spawn threads which is not allowed through
 * options parsing. Callers should default to HttpConnectionPool::kUse unless they are calling
 * into the HttpClient before thread spawning is allowed.
 */
enum class HttpConnectionPool {
    kUse,
    kDoNotUse,
};
class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient(HttpConnectionPool pool) : _pool(pool) {}

    void allowInsecureHTTP(bool allow) final {
        _allowInsecure = allow;
    }

    void setHeaders(const std::vector<std::string>& headers) final {
        // Can't set on base handle because cURL doesn't deep-dup this field
        // and we don't want it getting overwritten while another thread is using it.

        _headers = headers;
    }

    HttpReply request(HttpMethod method,
                      StringData url,
                      ConstDataRange cdr = {nullptr, 0}) const final {
        auto protocol = _allowInsecure ? Protocols::kHttpOrHttps : Protocols::kHttpsOnly;
        if (_pool == HttpConnectionPool::kUse) {
            static CurlPool factory;

            auto server = exactHostAndPortFromUrl(url);

            auto swHandle(factory.get(server, protocol));
            uassertStatusOK(swHandle.getStatus());

            CurlHandle handle(std::move(swHandle.getValue()));
            try {
                auto reply = request(handle.get(), method, url, cdr);
                handle.indicateSuccess();
                return reply;
            } catch (DBException& e) {
                handle.indicateFailure(e.toStatus());
                throw;
            }
        } else {
            // Make a request with a non-pooled handle. This is needed during server startup when
            // thread spawning is not allowed which is required by the thread pool.
            auto handle = createCurlEasyHandle(protocol);
            return request(handle.get(), method, url, cdr);
        }
    }

private:
    HttpReply request(CURL* handle, HttpMethod method, StringData url, ConstDataRange cdr) const {
        uassert(ErrorCodes::InternalError, "Curl initialization failed", handle);

        curl_easy_setopt(handle, CURLOPT_TIMEOUT, longSeconds(_timeout));

        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, longSeconds(_connectTimeout));

        BufReader bufReader(cdr.data(), cdr.length());
        switch (method) {
            case HttpMethod::kGET:
                uassert(ErrorCodes::BadValue,
                        "Request body not permitted with GET requests",
                        cdr.length() == 0);
                // Per https://curl.se/libcurl/c/CURLOPT_POST.html
                // We need to reset the type of request we want to make when reusing the request
                curl_easy_setopt(handle, CURLOPT_HTTPGET, 1);
                break;
            case HttpMethod::kPOST:
                curl_easy_setopt(handle, CURLOPT_PUT, 0);
                curl_easy_setopt(handle, CURLOPT_POST, 1);

                curl_easy_setopt(handle, CURLOPT_READFUNCTION, ReadMemoryCallback);
                curl_easy_setopt(handle, CURLOPT_READDATA, &bufReader);
                curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)bufReader.remaining());

                curl_easy_setopt(handle, CURLOPT_SEEKFUNCTION, SeekMemoryCallback);
                curl_easy_setopt(handle, CURLOPT_SEEKDATA, &bufReader);
                break;
            case HttpMethod::kPUT:
                curl_easy_setopt(handle, CURLOPT_POST, 0);
                curl_easy_setopt(handle, CURLOPT_PUT, 1);

                curl_easy_setopt(handle, CURLOPT_READFUNCTION, ReadMemoryCallback);
                curl_easy_setopt(handle, CURLOPT_READDATA, &bufReader);
                curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, (long)bufReader.remaining());

                curl_easy_setopt(handle, CURLOPT_SEEKFUNCTION, SeekMemoryCallback);
                curl_easy_setopt(handle, CURLOPT_SEEKDATA, &bufReader);
                break;
            default:
                MONGO_UNREACHABLE;
        }

        const auto urlString = url.toString();
        curl_easy_setopt(handle, CURLOPT_URL, urlString.c_str());

        DataBuilder dataBuilder(4096), headerBuilder(4096);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &dataBuilder);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &headerBuilder);

        curl_slist* chunk = curl_slist_append(nullptr, "Connection: keep-alive");
        for (const auto& header : _headers) {
            chunk = curl_slist_append(chunk, header.c_str());
        }
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, chunk);
        CurlSlist _headers(chunk);

        CURLcode result = curl_easy_perform(handle);
        uassert(ErrorCodes::OperationFailed,
                str::stream() << "Bad HTTP response from API server: "
                              << curl_easy_strerror(result),
                result == CURLE_OK);

        long statusCode;
        result = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &statusCode);
        uassert(ErrorCodes::OperationFailed,
                str::stream() << "Unexpected error retrieving response: "
                              << curl_easy_strerror(result),
                result == CURLE_OK);


        return HttpReply(statusCode, std::move(headerBuilder), std::move(dataBuilder));
    }

private:
    std::vector<std::string> _headers;

    HttpConnectionPool _pool;

    bool _allowInsecure{false};
};

class HttpClientProviderImpl : public HttpClientProvider {
public:
    HttpClientProviderImpl() {
        registerHTTPClientProvider(this);
    }

    std::unique_ptr<HttpClient> create() final {
        invariant(curlLibraryManager.isInitialized());
        return std::make_unique<CurlHttpClient>(HttpConnectionPool::kUse);
    }

    std::unique_ptr<HttpClient> createWithoutConnectionPool() final {
        invariant(curlLibraryManager.isInitialized());
        return std::make_unique<CurlHttpClient>(HttpConnectionPool::kDoNotUse);
    }

    BSONObj getServerStatus() final {
        invariant(curlLibraryManager.isInitialized());
        BSONObjBuilder info;
        info.append("type", "curl");

        {
            BSONObjBuilder v(info.subobjStart("compiled"));
            v.append("version", LIBCURL_VERSION);
            v.append("version_num", LIBCURL_VERSION_NUM);
        }

        {
            auto* curl_info = curl_version_info(CURLVERSION_NOW);

            BSONObjBuilder v(info.subobjStart("running"));
            v.append("version", curl_info->version);
            v.append("version_num", static_cast<int>(curl_info->version_num));
        }

        return info.obj();
    }

} provider;

}  // namespace

MONGO_INITIALIZER_GENERAL(CurlLibraryManager,
                          (),
                          ("BeginStartupOptionParsing", "NativeSaslClientContext"))
(InitializerContext* context) {
    uassertStatusOK(curlLibraryManager.initialize());
}


}  // namespace mongo
