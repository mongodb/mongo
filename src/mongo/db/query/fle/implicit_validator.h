// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/*
 * Generate a match expression from a list of encrypted fields.
 * Each path in the encrypted fields list generates a match expression that
 * verifies whether a document contains that path, and if so, whether its value
 * has the correct FLE2 encrypted BinData type. The returned match expression
 * is the individual expressions for each path, combined together in an $and
 * expression.
 * Supplying an empty list generates an always true match expression.
 */
StatusWithMatchExpression generateMatchExpressionFromEncryptedFields(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::vector<EncryptedField>& encryptedFields);

}  // namespace mongo
