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

#include "mongo/db/query/view_response_formatter.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <limits>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

static const NamespaceString testNss = NamespaceString::createNamespaceString_forTest("db.col");
static const CursorId testCursor(1);

TEST(ViewResponseFormatter, GetCountValueInitialResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << 7)});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    ASSERT_EQ(7, formatter.getCountValue(boost::none));
}

TEST(ViewResponseFormatter, GetCountValueMaxIntInitialResponse) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << std::numeric_limits<int>::max())});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    ASSERT_EQ(std::numeric_limits<int>::max(), formatter.getCountValue(boost::none));
}

TEST(ViewResponseFormatter, GetCountValueMaxLongInitialResponse) {
    CursorResponse cr(
        testNss, testCursor, {BSON("count" << std::numeric_limits<long long>::max())});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    ASSERT_EQ(std::numeric_limits<long long>::max(), formatter.getCountValue(boost::none));
}

TEST(ViewResponseFormatter, GetCountValueIntSubsequentResponse) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << 42)});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    ASSERT_EQ(42, formatter.getCountValue(boost::none));
}

TEST(ViewResponseFormatter, GetCountValueLongSubsequentResponse) {
    CursorResponse cr(
        testNss, testCursor, {BSON("count" << std::numeric_limits<long long>::max())});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    ASSERT_EQ(std::numeric_limits<long long>::max(), formatter.getCountValue(boost::none));
}

TEST(ViewResponseFormatter, GetCountValueTenantIdInitialResponse) {
    const TenantId tenantId(OID::gen());
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, testNss.toString_forTest());

    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        CursorResponse cr(nss, testCursor, {BSON("count" << 7)});
        ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
        ASSERT_EQ(7, formatter.getCountValue(tenantId));
    }
}


TEST(ViewResponseFormatter, GetCountValueEmptyInitialResponse) {
    CursorResponse cr(testNss, testCursor, {});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    ASSERT_EQ(0, formatter.getCountValue(boost::none));
}

TEST(ViewResponseFormatter, GetCountValueFails) {
    ViewResponseFormatter formatter(fromjson("{ok: 0, errmsg: 'bad value', code: 2}"));
    ASSERT_THROWS_CODE(formatter.getCountValue(boost::none), DBException, ErrorCodes::BadValue);
}

TEST(ViewResponseFormatter, FormatInitialCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << 7)});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    formatter.appendAsCountResponse(&builder, boost::none);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 7, ok: 1}"), bob);
    ASSERT_EQ(BSONType::numberInt, bob.getField("n"_sd).type());
}

TEST(ViewResponseFormatter, FormatInitialCountResponseWithNumberInt) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << std::numeric_limits<int>::max())});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    formatter.appendAsCountResponse(&builder, boost::none);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(BSON("n" << std::numeric_limits<int>::max() << "ok" << 1), bob);
    ASSERT_EQ(BSONType::numberInt, bob.getField("n"_sd).type());
}

TEST(ViewResponseFormatter, FormatInitialCountResponseWithNumberLong) {
    CursorResponse cr(
        testNss, testCursor, {BSON("count" << std::numeric_limits<long long>::max())});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    formatter.appendAsCountResponse(&builder, boost::none);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(BSON("n" << std::numeric_limits<long long>::max() << "ok" << 1), bob);
    ASSERT_EQ(BSONType::numberLong, bob.getField("n"_sd).type());
}

TEST(ViewResponseFormatter, FormatSubsequentCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << 7)});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    BSONObjBuilder builder;
    formatter.appendAsCountResponse(&builder, boost::none);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 7, ok: 1}"), bob);
    ASSERT_EQ(BSONType::numberInt, bob.getField("n"_sd).type());
}

TEST(ViewResponseFormatter, FormatSubsequentCountResponseWithLong) {
    CursorResponse cr(
        testNss, testCursor, {BSON("count" << std::numeric_limits<long long>::max())});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    BSONObjBuilder builder;
    formatter.appendAsCountResponse(&builder, boost::none);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(BSON("n" << std::numeric_limits<long long>::max() << "ok" << 1), bob);
    ASSERT_EQ(BSONType::numberLong, bob.getField("n"_sd).type());
}

TEST(ViewResponseFormatter, FormatInitialCountResponseWithTenantIdSuccessfully) {
    const TenantId tenantId(OID::gen());
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, testNss.toString_forTest());

    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        CursorResponse cr(nss, testCursor, {BSON("count" << 7)});
        ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
        BSONObjBuilder builder;
        formatter.appendAsCountResponse(&builder, tenantId);
        BSONObj bob = builder.obj();
        ASSERT_BSONOBJ_EQ(fromjson("{'n': 7, ok: 1}"), bob);
        ASSERT_EQ(BSONType::numberInt, bob.getField("n"_sd).type());
    }
}

TEST(ViewResponseFormatter, FormatEmptyInitialCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    formatter.appendAsCountResponse(&builder, boost::none);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 0, ok: 1}"), bob);
    ASSERT_EQ(BSONType::numberInt, bob.getField("n"_sd).type());
}

TEST(ViewResponseFormatter, FormatEmptySubsequentCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    BSONObjBuilder builder;
    formatter.appendAsCountResponse(&builder, boost::none);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 0, ok: 1}"), bob);
    ASSERT_EQ(BSONType::numberInt, bob.getField("n"_sd).type());
}

TEST(ViewResponseFormatter, FormatFailedCountResponseFails) {
    ViewResponseFormatter formatter(fromjson("{ok: 0, errmsg: 'bad value', code: 2}"));
    BSONObjBuilder builder;
    ASSERT_THROWS_CODE(
        formatter.appendAsCountResponse(&builder, boost::none), DBException, ErrorCodes::BadValue);
    BSONObj bob = builder.obj();
    ASSERT_BSONOBJ_EQ(bob, BSONObj());
}

TEST(ViewResponseFormatter, FormatInitialDistinctResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {fromjson("{_id: null, distinct: [5, 9]}")});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder, boost::none));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [5, 9], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatSubsequentDistinctResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {fromjson("{_id: null, distinct: [5, 9]}")});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder, boost::none));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [5, 9], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatInitialDistinctResponseWithTenantIdSuccessfully) {
    const TenantId tenantId(OID::gen());
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, testNss.toString_forTest());

    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        CursorResponse cr(nss, testCursor, {fromjson("{_id: null, distinct: [5, 9]}")});
        ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
        BSONObjBuilder builder;
        ASSERT_OK(formatter.appendAsDistinctResponse(&builder, tenantId));
        ASSERT_BSONOBJ_EQ(fromjson("{values: [5, 9], ok: 1}"), builder.obj());
    }
}

TEST(ViewResponseFormatter, FormatEmptyDistinctValuesSuccessfully) {
    CursorResponse cr(testNss, testCursor, {fromjson("{_id: null, distinct: []}")});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder, boost::none));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatEmptyDistinctBatchSuccessfully) {
    CursorResponse cr(testNss, testCursor, {});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder, boost::none));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatFailedDistinctResponseFails) {
    ViewResponseFormatter formatter(fromjson("{ok: 0, errmsg: 'bad value', code: 2}"));
    BSONObjBuilder builder;
    ASSERT_NOT_OK(formatter.appendAsDistinctResponse(&builder, boost::none));
    ASSERT_BSONOBJ_EQ(builder.obj(), BSONObj());
}

}  // namespace
}  // namespace mongo
