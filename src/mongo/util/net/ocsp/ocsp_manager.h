// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once


#include "mongo/base/data_range.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/http_client.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

enum class OCSPPurpose { kClientVerify, kStaple };

class OCSPManager {

public:
    OCSPManager();

    static void start(ServiceContext* service);

    static void shutdown(ServiceContext* service);

    static OCSPManager* get(ServiceContext* service);

    Future<std::vector<uint8_t>> requestStatus(std::vector<uint8_t> data,
                                               std::string_view responderURI,
                                               OCSPPurpose direction);

private:
    void startThreadPool();

private:
    std::unique_ptr<HttpClient> _tlsClientHttp;
    std::unique_ptr<HttpClient> _tlsServerHttp;

    std::unique_ptr<ThreadPool> _pool;
};

}  // namespace mongo
