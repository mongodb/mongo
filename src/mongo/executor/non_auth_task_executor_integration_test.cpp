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

#include "mongo/platform/basic.h"

#include "mongo/executor/non_auth_task_executor.h"

#include "mongo/db/service_context.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

namespace mongo {
namespace executor {
namespace {

// Basic test that the non auth task executor is actually set up and works
TEST(NonAuthTaskExecutor, Basic) {
    ServiceContext::UniqueServiceContext svc = ServiceContext::make();
    auto exec = getNonAuthTaskExecutor(svc.get());

    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             "admin",
                             BSON("isMaster" << 1),
                             BSONObj(),
                             nullptr);

    auto pf = makePromiseFuture<void>();

    ASSERT(exec->scheduleRemoteCommand(std::move(rcr),
                                       [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                                           if (args.response.isOK()) {
                                               pf.promise.emplaceValue();
                                           } else {
                                               pf.promise.setError(args.response.status);
                                           }
                                       })
               .isOK());

    ASSERT_OK(pf.future.getNoThrow());
}

TEST(NonAuthTaskExecutor, NotUsingIsNonFatal) {
    // Test purposefully makes a service context and immediately throws it away to ensure that we
    // can construct and destruct a service context (which is decorated with a non auth task
    // executor) even if we never call startup().
    ServiceContext::make();
}

}  // namespace
}  // namespace executor
}  // namespace mongo
