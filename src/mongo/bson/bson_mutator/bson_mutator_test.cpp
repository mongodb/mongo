// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "bson_mutator.h"

#include "mongo/util/shared_buffer.h"

namespace mongo {
namespace {

void TestBSONDomain(ConstSharedBuffer input) {
    auto obj = BSONObj(input);
    std::cout << "BSON(" << obj.toString() << ")" << std::endl;
    for (const auto& elem : obj) {
        std::cout << "Name: " << elem.fieldName() << " | Type: " << elem.type() << std::endl;
    }
}

#define X(Camel, cpptype, bsontype, defaultdomain) .With##Camel(#Camel)

FUZZ_TEST(ArbitraryBSONFFuzz, TestBSONDomain)
    .WithDomains(fuzztest::Arbitrary<mongo::ConstSharedBuffer>()
                     BSON_MUTATOR_EXPAND_FIELD_TYPES(X));

}  // namespace
}  // namespace mongo
