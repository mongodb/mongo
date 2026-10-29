// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

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
    std::unique_ptr<CanonicalQuery> canonicalize(std::string_view queryStr);
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
