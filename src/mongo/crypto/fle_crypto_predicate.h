// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_tokens.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/modules.h"

#include <functional>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ConstFLE2TagAndEncryptedMetadataBlock;
class EncryptedPredicateEvaluatorV2 {
public:
    EncryptedPredicateEvaluatorV2(std::vector<ServerZerosEncryptionToken> zerosTokens);

    EncryptedPredicateEvaluatorV2() {}

    /**
     * Evaluates an encrypted predicate (equality or range) over an encrypted value.
     *
     * Returns a boolean indicator.
     */
    bool evaluate(Value fieldValue,
                  EncryptedBinDataType indexedValueType,
                  std::function<std::vector<ConstFLE2TagAndEncryptedMetadataBlock>(ConstDataRange)>
                      extractMetadataBlocks) const;

    std::vector<ServerZerosEncryptionToken> zerosDecryptionTokens() const {
        return _zerosDecryptionTokens;
    }

private:
    std::vector<ServerZerosEncryptionToken> _zerosDecryptionTokens;
};

}  // namespace mongo
