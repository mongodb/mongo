/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/free_mon/free_mon_http.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

class CurlLibraryManager {
public:
    ~CurlLibraryManager() {
        curl_global_cleanup();
    }

    bool initialize() {
        CURLcode ret = curl_global_init(CURL_GLOBAL_ALL);
        if (ret != CURLE_OK) {
            error() << "Failed to initialize CURL: " << static_cast<int64_t>(ret);
            return false;
        }

        curl_version_info_data* version_data = curl_version_info(CURLVERSION_NOW);
        if (!(version_data->features & CURL_VERSION_SSL)) {
            error() << "Curl lacks SSL support, cannot continue";
            return false;
        }

        return true;
    }
};

CurlLibraryManager curlLibraryManager;

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
        invariant(cdrc->advance(readSize).isOK());
        ret = readSize;
    }

    return ret;
}

class FreeMonCurlHttpClient : public FreeMonHttpClientInterface {
public:
    explicit FreeMonCurlHttpClient(std::unique_ptr<executor::ThreadPoolTaskExecutor> executor)
        : _executor(std::move(executor)) {}

    ~FreeMonCurlHttpClient() final = default;

    Future<std::vector<uint8_t>> postAsync(StringData url, const BSONObj obj) final {
        auto pf = makePromiseFuture<std::vector<uint8_t>>();
        std::string urlString(url.toString());

        auto status =
            _executor->scheduleWork([ shared_promise = pf.promise.share(), urlString, obj ](
                const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
                doPost(shared_promise, urlString, obj);
            });

        uassertStatusOK(status);
        return std::move(pf.future);
    }

private:
    static void doPost(SharedPromise<std::vector<uint8_t>> shared_promise,
                       const std::string& urlString,
                       const BSONObj& obj) {
        try {
            ConstDataRange data(obj.objdata(), obj.objdata() + obj.objsize());

            ConstDataRangeCursor cdrc(data);

            std::unique_ptr<CURL, void (*)(CURL*)> myHandle(curl_easy_init(), curl_easy_cleanup);

            if (!myHandle) {
                shared_promise.setError({ErrorCodes::InternalError, "Curl initialization failed"});
                return;
            }

            curl_easy_setopt(myHandle.get(), CURLOPT_URL, urlString.c_str());
            curl_easy_setopt(myHandle.get(), CURLOPT_POST, 1);

            // Allow http only if test commands are enabled
            if (getTestCommandsEnabled()) {
                curl_easy_setopt(
                    myHandle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
            } else {
                curl_easy_setopt(myHandle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
            }

            curl_easy_setopt(myHandle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
            curl_easy_setopt(myHandle.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

            DataBuilder dataBuilder(4096);

            curl_easy_setopt(myHandle.get(), CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(myHandle.get(), CURLOPT_WRITEDATA, &dataBuilder);

            curl_easy_setopt(myHandle.get(), CURLOPT_READFUNCTION, ReadMemoryCallback);
            curl_easy_setopt(myHandle.get(), CURLOPT_READDATA, &cdrc);
            curl_easy_setopt(myHandle.get(), CURLOPT_POSTFIELDSIZE, (long)cdrc.length());

            // CURLOPT_EXPECT_100_TIMEOUT_MS??
            curl_easy_setopt(myHandle.get(), CURLOPT_CONNECTTIMEOUT, kConnectionTimeoutSeconds);
            curl_easy_setopt(myHandle.get(), CURLOPT_TIMEOUT, kTotalRequestTimeoutSeconds);

#if LIBCURL_VERSION_NUM > 0x072200
            // Requires >= 7.34.0
            curl_easy_setopt(myHandle.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#endif
            curl_easy_setopt(myHandle.get(), CURLOPT_FOLLOWLOCATION, 0);

            curl_easy_setopt(myHandle.get(), CURLOPT_NOSIGNAL, 1);
            // TODO: consider making this configurable            If server log level > 3
            // curl_easy_setopt(myHandle.get(), CURLOPT_VERBOSE, 1);
            // curl_easy_setopt(myHandle.get(), CURLOPT_DEBUGFUNCTION , ???);

            curl_slist* chunk = nullptr;
            chunk = curl_slist_append(chunk, "Content-Type: application/octet-stream");
            chunk = curl_slist_append(chunk, "Accept: application/octet-stream");

            // Send the empty expect because we do not need the server to respond with 100-Contine
            chunk = curl_slist_append(chunk, "Expect:");

            std::unique_ptr<curl_slist, void (*)(curl_slist*)> chunkHolder(chunk,
                                                                           curl_slist_free_all);

            curl_easy_setopt(myHandle.get(), CURLOPT_HTTPHEADER, chunk);

            CURLcode result = curl_easy_perform(myHandle.get());
            if (result != CURLE_OK) {
                shared_promise.setError({ErrorCodes::OperationFailed,
                                         str::stream() << "Bad HTTP response from API server: "
                                                       << curl_easy_strerror(result)});
                return;
            }

            long statusCode;
            result = curl_easy_getinfo(myHandle.get(), CURLINFO_RESPONSE_CODE, &statusCode);
            if (result != CURLE_OK) {
                shared_promise.setError({ErrorCodes::OperationFailed,
                                         str::stream() << "Unexpected error retrieving response: "
                                                       << curl_easy_strerror(result)});
                return;
            }

            if (statusCode != 200) {
                shared_promise.setError(Status(
                    ErrorCodes::OperationFailed,
                    str::stream() << "Unexpected http status code from server: " << statusCode));
                return;
            }

            auto d = dataBuilder.getCursor();
            shared_promise.emplaceValue(std::vector<uint8_t>(d.data(), d.data() + d.length()));
        } catch (...) {
            shared_promise.setError(exceptionToStatus());
        }
    }

private:
    std::unique_ptr<executor::ThreadPoolTaskExecutor> _executor{};
};

}  // namespace

std::unique_ptr<FreeMonHttpClientInterface> createFreeMonHttpClient(
    std::unique_ptr<executor::ThreadPoolTaskExecutor> executor) {

    if (!curlLibraryManager.initialize()) {
        return nullptr;
    }

    return std::make_unique<FreeMonCurlHttpClient>(std::move(executor));
}

}  // namespace mongo
