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

#include <boost/container/small_vector.hpp>
#include <boost/optional/optional.hpp>

// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"

#include "mongo/base/string_data.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(FLEUtils, FindMatchingEncryptedField) {
    FieldRef fieldAbc("a.b.c"_sd);
    FieldRef fieldFoo("foo"_sd);
    FieldRef fieldXyz("x.y.z"_sd);
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
