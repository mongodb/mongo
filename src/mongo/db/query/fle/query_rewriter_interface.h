/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

class FLETagQueryInterface;

namespace fle {
enum class EncryptedCollScanMode {
    // Always use high cardinality filters, used by tests
    kForceAlways,

    // Use high cardinality mode if $in rewrites do not fit in the
    // internalQueryFLERewriteMemoryLimit memory limit
    kUseIfNeeded,

    // Do not rewrite into high cardinality filter, throw exceptions instead
    // Some contexts like upsert do not support $expr
    kDisallow,
};

/**
 * Low Selectivity rewrites use $expr which is not supported in all commands such as upserts.
 */
enum class EncryptedCollScanModeAllowed {
    kAllow,
    kDisallow,
};

/**
 * Pure virtual class that allows encrypted predicate rewrites to be unit tested independently from
 * the actual server rewrite.
 */
class QueryRewriterInterface {
public:
    virtual ~QueryRewriterInterface() {}
    virtual FLETagQueryInterface* getTagQueryInterface() const = 0;
    virtual const NamespaceString& getESCNss() const = 0;

    virtual EncryptedCollScanMode getEncryptedCollScanMode() const = 0;
    virtual ExpressionContext* getExpressionContext() const = 0;
};
}  // namespace fle
}  // namespace mongo
