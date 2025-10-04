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

#include "mongo/db/index/expression_params.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

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
