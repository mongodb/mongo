// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/expression_params.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/index_names.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"


namespace mongo {

void ExpressionParams::parseHashParams(const BSONObj& infoObj,
                                       int* versionOut,
                                       BSONObj* keyPattern) {
    // In case we have hashed indexes based on other hash functions in the future, we store
    // a hashVersion number. If hashVersion changes, "makeSingleHashKey" will need to change
    // accordingly.  Defaults to 0 if "hashVersion" is not included in the index spec or if
    // the value of "hashversion" is not a number
    *versionOut = infoObj["hashVersion"].numberInt();

    // Extract and validate the index key pattern
    int numHashFields = 0;
    *keyPattern = infoObj.getObjectField("key");
    for (auto&& indexField : *keyPattern) {
        // The 'indexField' can either be ascending (1), descending (-1), or HASHED. Any other field
        // types should have failed validation while parsing.
        invariant(indexField.isNumber() || (indexField.valueStringData() == IndexNames::HASHED));
        numHashFields += (indexField.isNumber()) ? 0 : 1;
    }
    // We shouldn't be here if there are no hashed fields in the index.
    invariant(numHashFields > 0);
    uassert(31303,
            str::stream() << "A maximum of one index field is allowed to be hashed but found "
                          << numHashFields << " for 'key' " << *keyPattern,
            numHashFields == 1);
}
}  // namespace mongo
