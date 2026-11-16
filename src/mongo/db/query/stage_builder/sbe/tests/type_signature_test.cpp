// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/type_signature.h"

#include "mongo/unittest/unittest.h"

namespace mongo::stage_builder {
namespace {

TEST(TypeSignatureTest, DefaultConstructedIsAnyType) {
    TypeSignature defaultInitialized;
    ASSERT_EQ(defaultInitialized, TypeSignature::kAnyType);
    ASSERT_EQ(TypeSignature{}, TypeSignature::kAnyType);
    ASSERT_EQ(TypeSignature(), TypeSignature::kAnyType);
    ASSERT_FALSE(TypeSignature{}.isEmpty());
}

TEST(TypeSignatureTest, DefaultConstructedMemberIsAnyType) {
    struct Holder {
        TypeSignature sig;
    };
    ASSERT_EQ(Holder{}.sig, TypeSignature::kAnyType);
}

TEST(TypeSignatureTest, NoTypeIsStillEmpty) {
    ASSERT_TRUE(TypeSignature::kNoType.isEmpty());
    ASSERT_NE(TypeSignature::kNoType, TypeSignature{});
}

TEST(TypeSignatureTest, OptTypeSignatureOfDefaultHasValue) {
    // A default-constructed signature must not be mistaken for the sentinel meaning "no value".
    OptTypeSignature fromDefault{TypeSignature{}};
    ASSERT_TRUE(fromDefault.has_value());
    ASSERT_EQ(*fromDefault, TypeSignature::kAnyType);

    OptTypeSignature absent;
    ASSERT_FALSE(absent.has_value());
}

}  // namespace
}  // namespace mongo::stage_builder
