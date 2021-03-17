/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#pragma once


#include "mongo/base/data_range.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/http_client.h"

namespace mongo {

enum class OCSPPurpose { kClientVerify, kStaple };

class OCSPManager {

public:
    OCSPManager();

    static void start(ServiceContext* service);

    static void shutdown(ServiceContext* service);

    static OCSPManager* get(ServiceContext* service);

    Future<std::vector<uint8_t>> requestStatus(std::vector<uint8_t> data,
                                               StringData responderURI,
                                               OCSPPurpose direction);

private:
    void startThreadPool();

private:
    std::unique_ptr<HttpClient> _tlsClientHttp;
    std::unique_ptr<HttpClient> _tlsServerHttp;

    std::unique_ptr<ThreadPool> _pool;
};

}  // namespace mongo
