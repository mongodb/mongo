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

#include <array>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/uuid.h"

namespace mongo {

class EncryptedPredicateEvaluatorV2 {
public:
    EncryptedPredicateEvaluatorV2(std::vector<ServerZerosEncryptionToken> zerosTokens);

    EncryptedPredicateEvaluatorV2() {}

    /**
     * Evaluates an encrypted predicate (equality or range) over an encrypted value.
     *
     * Returns a boolean indicator.
     */
    bool evaluate(
        Value fieldValue,
        EncryptedBinDataType indexedValueType,
        std::function<std::vector<ConstDataRange>(ConstDataRange)> extractMetadataBlocks) const;

    std::vector<ServerZerosEncryptionToken> zerosDecryptionTokens() const {
        return _zerosDecryptionTokens;
    }

private:
    std::vector<ServerZerosEncryptionToken> _zerosDecryptionTokens;
};

}  // namespace mongo
