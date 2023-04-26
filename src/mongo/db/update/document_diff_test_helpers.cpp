/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/update/document_diff_test_helpers.h"

#include "mongo/bson/json.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/platform/random.h"

namespace mongo::doc_diff {

BSONObj createObjWithLargePrefix(const std::string& suffix) {
    const static auto largeObj = BSON("prefixLargeField" << std::string(200, 'a'));
    return largeObj.addFields(fromjson(suffix));
}

std::string getFieldName(int level, int fieldNum) {
    return str::stream() << "f" << level << fieldNum;
}

Value getScalarFieldValue(PseudoRandom* rng) {
    switch (rng->nextInt32(10)) {
        case 0:
            return Value("val"_sd);
        case 1:
            return Value(BSONNULL);
        case 2:
            return Value(-1LL);
        case 3:
            return Value(0);
        case 4:
            return Value(1.10);
        case 5:
            return Value(false);
        case 6:
            return Value(BSONRegEx("p"));
        case 7:
            return Value(Date_t());
        case 8:
            return Value(UUID::gen());
        case 9:
            return Value(BSONBinData("asdf", 4, BinDataGeneral));
        default:
            MONGO_UNREACHABLE;
    }
}

BSONObj generateDoc(PseudoRandom* rng, MutableDocument* doc, int depthLevel) {
    // Append a large field at each level so that the likelihood of generating a sub-diff is high.
    auto largeFieldObj = createObjWithLargePrefix("{}");
    doc->addField("prefixLargeField", Value(largeFieldObj.firstElement()));

    // Reduce the probabilty of generated nested objects as we go deeper. After depth level 6, we
    // should not be generating anymore nested objects.
    const double subObjProbability = 0.3 - (depthLevel * 0.05);
    const double subArrayProbability = 0.2 - (depthLevel * 0.05);

    const int numFields = (5 - depthLevel) + rng->nextInt32(4);
    for (int fieldNum = 0; fieldNum < numFields; ++fieldNum) {
        const auto fieldName = getFieldName(depthLevel, fieldNum);
        auto num = rng->nextCanonicalDouble();
        if (num <= subObjProbability) {
            MutableDocument subDoc;
            doc->addField(fieldName, Value(generateDoc(rng, &subDoc, depthLevel + 1)));
        } else if (num <= (subObjProbability + subArrayProbability)) {
            const auto arrayLength = rng->nextInt32(10);
            std::vector<Value> values;
            auto numSubObjs = 0;
            for (auto i = 0; i < arrayLength; i++) {
                // The probablilty of generating a sub-object goes down as we generate more
                // sub-objects and as the array position increases.
                if (!rng->nextInt32(2 + numSubObjs + i)) {
                    MutableDocument subDoc;

                    // Ensure that the depth is higher as we generate more sub-objects, to avoid
                    // bloating up the document size exponentially.
                    values.push_back(
                        Value(generateDoc(rng, &subDoc, depthLevel + 1 + numSubObjs * 2)));
                    ++numSubObjs;
                } else {
                    values.push_back(getScalarFieldValue(rng));
                }
            }

            doc->addField(fieldName, Value(values));
        } else {
            doc->addField(fieldName, getScalarFieldValue(rng));
        }
    }
    return doc->freeze().toBson();
}

BSONObj applyDiffTestHelper(BSONObj preImage,
                            BSONObj diff,
                            bool mustCheckExistenceForInsertOperations) {
    return applyDiff(preImage, diff, mustCheckExistenceForInsertOperations);
}

}  // namespace mongo::doc_diff
