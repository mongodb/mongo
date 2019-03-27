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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <cstddef>
#include <curl/curl.h>
#include <curl/easy.h>
#include <string>

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/http_client.h"

namespace mongo {

namespace {

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
        if (_share) {
            curl_share_cleanup(_share);
        }
        // Ordering matters: curl_global_cleanup() must happen last.
        if (_initialized) {
            curl_global_cleanup();
        }
    }

    Status initialize() {
        auto status = _initializeGlobal();
        if (!status.isOK()) {
            return status;
        }

        status = _initializeShare();
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    CURLSH* getShareHandle() const {
        return _share;
    }

private:
    Status _initializeGlobal() {
        if (_initialized) {
            return Status::OK();
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

    Status _initializeShare() {
        invariant(_initialized);
        if (_share) {
            return Status::OK();
        }

        _share = curl_share_init();
        curl_share_setopt(_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(_share, CURLSHOPT_USERDATA, &this->_shareMutex);
        curl_share_setopt(_share, CURLSHOPT_LOCKFUNC, _lockShare);
        curl_share_setopt(_share, CURLSHOPT_UNLOCKFUNC, _unlockShare);

        return Status::OK();
    }

    static void _lockShare(CURL*, curl_lock_data, curl_lock_access, void* ctx) {
        reinterpret_cast<stdx::mutex*>(ctx)->lock();
    }

    static void _unlockShare(CURL*, curl_lock_data, void* ctx) {
        reinterpret_cast<stdx::mutex*>(ctx)->unlock();
    }

private:
    bool _initialized = false;
    CURLSH* _share = nullptr;
    stdx::mutex _shareMutex;
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

    auto* cdrc = reinterpret_cast<ConstDataRangeCursor*>(instream);

    size_t ret = 0;

    if (cdrc->length() > 0) {
        size_t readSize = std::min(size * nitems, cdrc->length());
        memcpy(buffer, cdrc->data(), readSize);
        invariant(cdrc->advanceNoThrow(readSize).isOK());
        ret = readSize;
    }

    return ret;
}

struct CurlEasyCleanup {
    void operator()(CURL* handle) {
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }
};
using CurlHandle = std::unique_ptr<CURL, CurlEasyCleanup>;

struct CurlSlistFreeAll {
    void operator()(curl_slist* list) {
        if (list) {
            curl_slist_free_all(list);
        }
    }
};
using CurlSlist = std::unique_ptr<curl_slist, CurlSlistFreeAll>;

class CurlHttpClient : public HttpClient {
public:
    CurlHttpClient() {
        // Initialize a base handle with common settings.
        // Methods like requireHTTPS() will operate on this
        // base handle.
        _handle.reset(curl_easy_init());
        uassert(ErrorCodes::InternalError, "Curl initialization failed", _handle);

        curl_easy_setopt(_handle.get(), CURLOPT_CONNECTTIMEOUT, longSeconds(kConnectionTimeout));
        curl_easy_setopt(_handle.get(), CURLOPT_FOLLOWLOCATION, 0);
        curl_easy_setopt(_handle.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(_handle.get(), CURLOPT_NOSIGNAL, 1);
        curl_easy_setopt(_handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#ifdef CURLOPT_TCP_KEEPALIVE
        curl_easy_setopt(_handle.get(), CURLOPT_TCP_KEEPALIVE, 1);
#endif
        curl_easy_setopt(_handle.get(), CURLOPT_TIMEOUT, longSeconds(kTotalRequestTimeout));

#if LIBCURL_VERSION_NUM > 0x072200
        // Requires >= 7.34.0
        curl_easy_setopt(_handle.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#endif
        curl_easy_setopt(_handle.get(), CURLOPT_WRITEFUNCTION, WriteMemoryCallback);

        // TODO: CURLOPT_EXPECT_100_TIMEOUT_MS?
        // TODO: consider making this configurable
        // curl_easy_setopt(_handle.get(), CURLOPT_VERBOSE, 1);
        // curl_easy_setopt(_handle.get(), CURLOPT_DEBUGFUNCTION , ???);
    }

    ~CurlHttpClient() final = default;

    void allowInsecureHTTP(bool allow) final {
        if (allow) {
            curl_easy_setopt(_handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
        } else {
            curl_easy_setopt(_handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        }
    }

    void setHeaders(const std::vector<std::string>& headers) final {
        // Can't set on base handle because cURL doesn't deep-dup this field
        // and we don't want it getting overwritten while another thread is using it.
        _headers = headers;
    }

    void setTimeout(Seconds timeout) final {
        curl_easy_setopt(_handle.get(), CURLOPT_TIMEOUT, longSeconds(timeout));
    }

    void setConnectTimeout(Seconds timeout) final {
        curl_easy_setopt(_handle.get(), CURLOPT_CONNECTTIMEOUT, longSeconds(timeout));
    }

    DataBuilder get(StringData url) const final {
        // Make a local copy of the base handle for this request.
        CurlHandle myHandle(curl_easy_duphandle(_handle.get()));
        uassert(ErrorCodes::InternalError, "Curl initialization failed", myHandle);

        return doRequest(myHandle.get(), url);
    }

    DataBuilder post(StringData url, ConstDataRange cdr) const final {
        // Make a local copy of the base handle for this request.
        CurlHandle myHandle(curl_easy_duphandle(_handle.get()));
        uassert(ErrorCodes::InternalError, "Curl initialization failed", myHandle);

        curl_easy_setopt(myHandle.get(), CURLOPT_POST, 1);

        ConstDataRangeCursor cdrc(cdr);
        curl_easy_setopt(myHandle.get(), CURLOPT_READFUNCTION, ReadMemoryCallback);
        curl_easy_setopt(myHandle.get(), CURLOPT_READDATA, &cdrc);
        curl_easy_setopt(myHandle.get(), CURLOPT_POSTFIELDSIZE, (long)cdrc.length());

        return doRequest(myHandle.get(), url);
    }

private:
    /**
     * Helper for use with curl_easy_setopt which takes a vararg list,
     * and expects a long, not the long long durationCount() returns.
     */
    long longSeconds(Seconds tm) {
        return static_cast<long>(durationCount<Seconds>(tm));
    }

    DataBuilder doRequest(CURL* handle, StringData url) const {
        const auto urlString = url.toString();
        curl_easy_setopt(handle, CURLOPT_URL, urlString.c_str());
        curl_easy_setopt(handle, CURLOPT_SHARE, curlLibraryManager.getShareHandle());

        DataBuilder dataBuilder(4096);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &dataBuilder);

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

        uassert(ErrorCodes::OperationFailed,
                str::stream() << "Unexpected http status code from server: " << statusCode,
                statusCode == 200);

        return dataBuilder;
    }

private:
    CurlHandle _handle;
    std::vector<std::string> _headers;
};

}  // namespace

// Transitional API used by blockstore to trigger libcurl init
// until it's been migrated to use the HTTPClient API.
Status curlLibraryManager_initialize() {
    return curlLibraryManager.initialize();
}

std::unique_ptr<HttpClient> HttpClient::create() {
    uassertStatusOK(curlLibraryManager.initialize());
    return std::make_unique<CurlHttpClient>();
}

BSONObj HttpClient::getServerStatus() {

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
        v.append("version_num", curl_info->version_num);
    }

    return info.obj();
}

}  // namespace mongo
