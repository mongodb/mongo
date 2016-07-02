/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

/**
 * Test fixture which provides an ExpressionContext for use in testing.
 */
class AggregationContextFixture : public unittest::Test {
public:
    AggregationContextFixture()
        : _queryServiceContext(stdx::make_unique<QueryTestServiceContext>()),
          _opCtx(_queryServiceContext->makeOperationContext()),
          _expCtx(new ExpressionContext(
              _opCtx.get(), AggregationRequest(NamespaceString("unittests.pipeline_test"), {}))) {}

    boost::intrusive_ptr<ExpressionContext> getExpCtx() {
        return _expCtx.get();
    }

private:
    std::unique_ptr<QueryTestServiceContext> _queryServiceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};
}  // namespace mongo
