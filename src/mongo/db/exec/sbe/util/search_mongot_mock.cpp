/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/util/search_mongot_mock.h"

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"

namespace mongo::search_mongot_mock {

namespace {
ServiceContext::ConstructorActionRegisterer searchQueryMock{
    "searchQueryMock", {"searchQueryHelperRegisterer"}, [](ServiceContext* context) {
        invariant(context);
        getSearchHelpers(context) = std::make_unique<SearchMockHelperFunctions>();
    }};
}

boost::optional<executor::TaskExecutorCursor> SearchMockHelperFunctions::establishSearchCursor(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    const boost::optional<ExplainOptions::Verbosity>& explain,
    const BSONObj& query,
    CursorResponse&& response,
    boost::optional<long long> docsRequested,
    std::function<boost::optional<long long>()> calcDocsNeeded,
    const boost::optional<int>& protocolVersion,
    bool requiresSearchSequenceToken) {
    auto networkInterface = std::make_unique<executor::NetworkInterfaceMock>();
    auto testExecutor = executor::makeSharedThreadPoolTestExecutor(std::move(networkInterface));
    executor::RemoteCommandRequest req = executor::RemoteCommandRequest();
    req.opCtx = opCtx;

    executor::TaskExecutorCursor::Options opts;
    opts.preFetchNextBatch = false;
    return executor::TaskExecutorCursor(
        testExecutor, nullptr /* underlyingExec */, std::move(response), req, std::move(opts));
}
}  // namespace mongo::search_mongot_mock
