// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class EncryptedField;
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

    // EncryptedFieldConfig for the current namespace, or boost::none if validation isn't enabled.
    virtual boost::optional<const EncryptedFieldConfig&> getEncryptedFieldConfigForValidation()
        const {
        return boost::none;
    }
};
}  // namespace fle
}  // namespace mongo
