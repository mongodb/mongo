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

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search_helper.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo::search_mongot_mock {

/**
 * A class that contains methods that are mock implementations of mongot search.
 * This will be used in SearchCursorStage unit tests to avoid remote call to mongot or mongot_mock.
 */
class SearchMockHelperFunctions : public SearchDefaultHelperFunctions {
public:
    boost::optional<executor::TaskExecutorCursor> establishSearchCursor(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<UUID>& uuid,
        const boost::optional<ExplainOptions::Verbosity>& explain,
        const BSONObj& query,
        CursorResponse&& response,
        boost::optional<long long> docsRequested = boost::none,
        std::function<boost::optional<long long>()> calcDocsNeeded = nullptr,
        const boost::optional<int>& protocolVersion = boost::none) override;
};

}  // namespace mongo::search_mongot_mock
