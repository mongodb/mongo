/**
 * Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stub_mongo_process_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo {

/**
 * An ExpressionContext with a default OperationContext that can have state (like the resolved
 * namespace map) manipulated after construction. In contrast, a regular ExpressionContext requires
 * the resolved namespaces to be provided on construction and does not allow them to be subsequently
 * mutated.
 */
class ExpressionContextForTest : public ExpressionContext {
public:
    static constexpr TimeZoneDatabase* kNullTimeZoneDatabase = nullptr;

    ExpressionContextForTest()
        : ExpressionContextForTest(NamespaceString{"test"_sd, "namespace"_sd}) {}

    ExpressionContextForTest(NamespaceString nss)
        : ExpressionContext(
              std::move(nss), std::make_shared<StubMongoProcessInterface>(), kNullTimeZoneDatabase),
          _testOpCtx(_serviceContext.makeOperationContext()) {
        TimeZoneDatabase::set(_serviceContext.getServiceContext(),
                              stdx::make_unique<TimeZoneDatabase>());

        // As we don't have the TimeZoneDatabase prior to ExpressionContext construction, we must
        // initialize with a nullptr and set post-construction.
        timeZoneDatabase = TimeZoneDatabase::get(_serviceContext.getServiceContext());
        opCtx = _testOpCtx.get();
    }

    ExpressionContextForTest(OperationContext* opCtx, const AggregationRequest& request)
        : ExpressionContext(
              opCtx, request, nullptr, std::make_shared<StubMongoProcessInterface>(), {}, {}) {}

    /**
     * Sets the resolved definition for an involved namespace.
     */
    void setResolvedNamespace(const NamespaceString& nss, ResolvedNamespace resolvedNamespace) {
        _resolvedNamespaces[nss.coll()] = std::move(resolvedNamespace);
    }

private:
    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _testOpCtx;
};

}  // namespace mongo
