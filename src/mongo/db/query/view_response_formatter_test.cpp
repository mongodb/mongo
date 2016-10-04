/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static const NamespaceString testNss("db.col");
static const CursorId testCursor(1);

TEST(ViewResponseFormatter, FormatInitialCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << 7)});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsCountResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 7, ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatSubsequentCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {BSON("count" << 7)});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsCountResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 7, ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatEmptyInitialCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsCountResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 0, ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatEmptySubsequentCountResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsCountResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{'n': 0, ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatFailedCountResponseFails) {
    ViewResponseFormatter formatter(fromjson("{ok: 0, errmsg: 'bad value', code: 2}"));
    BSONObjBuilder builder;
    ASSERT_NOT_OK(formatter.appendAsCountResponse(&builder));
    ASSERT_BSONOBJ_EQ(builder.obj(), BSONObj());
}

TEST(ViewResponseFormatter, FormatInitialDistinctResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {fromjson("{_id: null, distinct: [5, 9]}")});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [5, 9], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatSubsequentDistinctResponseSuccessfully) {
    CursorResponse cr(testNss, testCursor, {fromjson("{_id: null, distinct: [5, 9]}")});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::SubsequentResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [5, 9], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatEmptyDistinctValuesSuccessfully) {
    CursorResponse cr(testNss, testCursor, {fromjson("{_id: null, distinct: []}")});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatEmptyDistinctBatchSuccessfully) {
    CursorResponse cr(testNss, testCursor, {});
    ViewResponseFormatter formatter(cr.toBSON(CursorResponse::ResponseType::InitialResponse));
    BSONObjBuilder builder;
    ASSERT_OK(formatter.appendAsDistinctResponse(&builder));
    ASSERT_BSONOBJ_EQ(fromjson("{values: [], ok: 1}"), builder.obj());
}

TEST(ViewResponseFormatter, FormatFailedDistinctResponseFails) {
    ViewResponseFormatter formatter(fromjson("{ok: 0, errmsg: 'bad value', code: 2}"));
    BSONObjBuilder builder;
    ASSERT_NOT_OK(formatter.appendAsDistinctResponse(&builder));
    ASSERT_BSONOBJ_EQ(builder.obj(), BSONObj());
}

}  // namespace
}  // namespace mongo
