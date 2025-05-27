/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
/**
 * Utility function for getting the encoding from a canonical query. It is used to cover both
 * cases with classic and SBE encoding based on wheter classic was enforced or wheter the query is
 * compatible with SBE.
 */
CanonicalQuery::QueryShapeString encodeKey(const CanonicalQuery& cq);

class CanonicalQueryTest : public unittest::Test {
public:
    CanonicalQueryTest() : _opCtx(_serviceContext.makeOperationContext()) {}

protected:
    OperationContext* opCtx() const {
        return _opCtx.get();
    }

    std::unique_ptr<CanonicalQuery> canonicalize(const BSONObj& queryObj);
    std::unique_ptr<CanonicalQuery> canonicalize(StringData queryStr);
    std::unique_ptr<CanonicalQuery> canonicalize(BSONObj query,
                                                 BSONObj sort,
                                                 BSONObj proj,
                                                 BSONObj collation);
    std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                                 const char* sortStr,
                                                 const char* projStr,
                                                 const char* collationStr);
    std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                                 const char* sortStr,
                                                 const char* projStr,
                                                 long long skip,
                                                 long long limit,
                                                 const char* hintStr,
                                                 const char* minStr,
                                                 const char* maxStr);
    std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                                 const char* sortStr,
                                                 const char* projStr,
                                                 long long skip,
                                                 long long limit,
                                                 const char* hintStr,
                                                 const char* minStr,
                                                 const char* maxStr,
                                                 bool explain);

    static const NamespaceString nss;

private:
    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
};
}  // namespace mongo
