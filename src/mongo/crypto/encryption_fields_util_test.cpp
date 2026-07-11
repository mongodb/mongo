// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/encryption_fields_util.h"

#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

TEST(FLEUtils, FindMatchingEncryptedField) {
    FieldRef fieldAbc("a.b.c"sv);
    FieldRef fieldFoo("foo"sv);
    FieldRef fieldXyz("x.y.z"sv);
    FieldRef fieldEmpty;
    std::vector<FieldRef> encryptedFields = {fieldAbc, fieldFoo, fieldEmpty, fieldXyz};

    // no-op if no encrypted fields
    auto result = findMatchingEncryptedField(fieldFoo, {});
    ASSERT_FALSE(result);

    // empty fields never match
    result = findMatchingEncryptedField(fieldEmpty, encryptedFields);
    ASSERT_FALSE(result);

    // no match
    result = findMatchingEncryptedField(FieldRef("foobar"), encryptedFields);
    ASSERT_FALSE(result);

    result = findMatchingEncryptedField(FieldRef("a.b.cd"), encryptedFields);
    ASSERT_FALSE(result);

    // prefix match
    result = findMatchingEncryptedField(FieldRef("a"), encryptedFields);
    ASSERT(result);
    ASSERT(result->keyIsPrefixOrEqual);
    ASSERT(result->encryptedField == fieldAbc);

    result = findMatchingEncryptedField(FieldRef("x.y"), encryptedFields);
    ASSERT(result);
    ASSERT(result->keyIsPrefixOrEqual);
    ASSERT(result->encryptedField == fieldXyz);

    result = findMatchingEncryptedField(FieldRef("foo.bar.baz"), encryptedFields);
    ASSERT(result);
    ASSERT_FALSE(result->keyIsPrefixOrEqual);
    ASSERT(result->encryptedField == fieldFoo);

    // exact match
    result = findMatchingEncryptedField(fieldFoo, encryptedFields);
    ASSERT(result);
    ASSERT(result->keyIsPrefixOrEqual);
    ASSERT(result->encryptedField == fieldFoo);
}

}  // namespace mongo
