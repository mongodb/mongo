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

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
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

    /**
     * If there is a global ServiceContext available, this constructor will adopt it. Otherwise, it
     * will internally create an owned QueryTestServiceContext. Similarly, if an OperationContext
     * already exists on the current client then it will be adopted, otherwise an owned OpCtx will
     * be created using the ServiceContext. The OpCtx will be set on the ExpressionContextForTest.
     * Defaults to using a namespace of "test.namespace".
     */
    ExpressionContextForTest()
        : ExpressionContextForTest(NamespaceString{"test"_sd, "namespace"_sd}) {}
    /**
     * If there is a global ServiceContext available, this constructor will adopt it. Otherwise, it
     * will internally create an owned QueryTestServiceContext. Similarly, if an OperationContext
     * already exists on the current client then it will be adopted, otherwise an owned OpCtx will
     * be created using the ServiceContext. The OpCtx will be set on the ExpressionContextForTest.
     */
    ExpressionContextForTest(NamespaceString nss)
        : ExpressionContext(nullptr,      // opCtx, nullptr while base class is constructed.
                            boost::none,  // explain
                            false,        // fromMongos,
                            false,        // needsMerge,
                            false,        // allowDiskUse,
                            false,        // bypassDocumentValidation,
                            false,        // isMapReduce
                            nss,
                            RuntimeConstants(Date_t::now(), Timestamp(1, 0)),
                            {},  // collator
                            std::make_shared<StubMongoProcessInterface>(),
                            {},  // resolvedNamespaces
                            {}   // collUUID
          ) {
        // If there is an existing global ServiceContext, adopt it. Otherwise, create a new context.
        // Similarly, we create a new OperationContext or adopt an existing context as appropriate.
        if (hasGlobalServiceContext()) {
            _serviceContext = getGlobalServiceContext();
            if (!Client::getCurrent()->getOperationContext()) {
                _testOpCtx = getGlobalServiceContext()->makeOperationContext(Client::getCurrent());
            }
        } else {
            _serviceContext = std::make_unique<QueryTestServiceContext>();
            _testOpCtx = stdx::get<std::unique_ptr<QueryTestServiceContext>>(_serviceContext)
                             ->makeOperationContext();
        }

        // Resolve the active OperationContext and set it on the ExpressionContextForTest.
        opCtx = _testOpCtx ? _testOpCtx.get() : Client::getCurrent()->getOperationContext();

        // As we don't have an OperationContext or TimeZoneDatabase prior to base class
        // ExpressionContext construction, we must resolve one. If there exists a TimeZoneDatabase
        // associated with the current ServiceContext, adopt it. Otherwise, create a
        // new one.
        _setTimeZoneDatabase();
    }

    /**
     * Constructor which sets the given OperationContext on the ExpressionContextForTest. This will
     * also resolve the ExpressionContextForTest's ServiceContext from the OperationContext.
     * Defaults to using a namespace of "test.namespace".
     */
    ExpressionContextForTest(OperationContext* opCtx)
        : ExpressionContextForTest(opCtx, NamespaceString{"test"_sd, "namespace"_sd}) {}

    /**
     * Constructor which sets the given OperationContext on the ExpressionContextForTest. This will
     * also resolve the ExpressionContextForTest's ServiceContext from the OperationContext.
     */
    ExpressionContextForTest(OperationContext* opCtx, NamespaceString nss)
        : ExpressionContext(opCtx,
                            boost::none,  // explain
                            false,        // fromMongos,
                            false,        // needsMerge,
                            false,        // allowDiskUse,
                            false,        // bypassDocumentValidation,
                            false,        // isMapReduce
                            nss,
                            RuntimeConstants(Date_t::now(), Timestamp(1, 0)),
                            {},  // collator
                            std::make_shared<StubMongoProcessInterface>(),
                            {},  // resolvedNamespaces
                            {}   // collUUID
                            ),
          _serviceContext(opCtx->getServiceContext()) {
        // Resolve the TimeZoneDatabase to be used by this ExpressionContextForTest.
        _setTimeZoneDatabase();
    }

    /**
     * Constructor which sets the given OperationContext on the ExpressionContextForTest. This will
     * also resolve the ExpressionContextForTest's ServiceContext from the OperationContext.
     */
    ExpressionContextForTest(OperationContext* opCtx, const AggregationRequest& request)
        : ExpressionContext(
              opCtx, request, nullptr, std::make_shared<StubMongoProcessInterface>(), {}, {}),
          _serviceContext(opCtx->getServiceContext()) {
        // Resolve the TimeZoneDatabase to be used by this ExpressionContextForTest.
        _setTimeZoneDatabase();
    }

    /**
     * Sets the resolved definition for an involved namespace.
     */
    void setResolvedNamespace(const NamespaceString& nss, ResolvedNamespace resolvedNamespace) {
        _resolvedNamespaces[nss.coll()] = std::move(resolvedNamespace);
    }

    ServiceContext* getServiceContext() {
        struct Visitor {
            auto operator()(ServiceContext* ctx) {
                return ctx;
            }
            auto operator()(const std::unique_ptr<QueryTestServiceContext>& ctx) {
                return ctx->getServiceContext();
            }
        };
        return stdx::visit(Visitor{}, _serviceContext);
    }

private:
    // In cases when there is a ServiceContext, if there already exists a TimeZoneDatabase
    // associated with the ServiceContext, adopt it. Otherwise, create a new one.
    void _setTimeZoneDatabase() {
        // In some cases, e.g. the user uses an OperationContextNoop which does _not_ provide a
        // ServiceContext to create this ExpressionContextForTest, then it shouldn't resolve any
        // timeZoneDatabase.
        if (auto* serviceContext = getServiceContext()) {
            if (!TimeZoneDatabase::get(serviceContext)) {
                TimeZoneDatabase::set(serviceContext, std::make_unique<TimeZoneDatabase>());
            }
            timeZoneDatabase = TimeZoneDatabase::get(serviceContext);
        }
    }

    stdx::variant<ServiceContext*, std::unique_ptr<QueryTestServiceContext>> _serviceContext;
    ServiceContext::UniqueOperationContext _testOpCtx;
};

}  // namespace mongo
