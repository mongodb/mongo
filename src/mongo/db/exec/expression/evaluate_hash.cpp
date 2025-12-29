/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/md5.h"
#include "mongo/util/text.h"

#include <common/xxhash.h>

namespace mongo {

namespace exec::expression {

namespace {

HashAlgorithm parseAlgorithm(Value algorithm) {
    uassert(10754001,
            str::stream() << "$hash requires that 'algorithm' be a string, found: "
                          << typeName(algorithm.getType()) << " with value "
                          << algorithm.toString(),
            algorithm.getType() == BSONType::string);

    static const StringDataMap<HashAlgorithm> stringToAlgorithm{
        {toStringData(HashAlgorithm::md5), HashAlgorithm::md5},
        {toStringData(HashAlgorithm::sha256), HashAlgorithm::sha256},
        {toStringData(HashAlgorithm::xxh64), HashAlgorithm::xxh64},
    };

    auto algorithmString = algorithm.getStringData();
    auto algorithmPair = stringToAlgorithm.find(algorithmString);

    uassert(10754002,
            str::stream() << "Currently, the only supported algorithms for $hash are 'md5', "
                             "'sha256' and 'xxh64', found: "
                          << algorithmString,
            algorithmPair != stringToAlgorithm.end());

    return algorithmPair->second;
}

}  // namespace

Value evaluate(const ExpressionHash& expr, const Document& root, Variables* variables) {
    auto input = expr.getInput().evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    auto inputType = input.getType();
    uassert(
        10754000,
        str::stream() << "$hash requires that 'input' be a valid UTF-8 string or binData, found: "
                      << typeName(input.getType()) << " with value " << input.toString(),
        (inputType == BSONType::string && isValidUTF8(input.getStringData())) ||
            inputType == BSONType::binData);

    auto algorithm = parseAlgorithm(expr.getAlgorithm().evaluate(root, variables));

    ConstDataRange inputBytes = inputType == BSONType::string
        ? ConstDataRange(input.getStringData().data(), input.getStringData().size())
        : ConstDataRange(static_cast<const char*>(input.getBinData().data),
                         input.getBinData().length);

    switch (algorithm) {
        case HashAlgorithm::md5: {
            uassert(10754003,
                    "The md5 algorithm for $hash is disabled while in FIPS mode",
                    !sslGlobalParams.sslFIPSMode);

            // MD5 is a deprecated hashing algorithm but supported for users migrating from hex_md5
            // in $function and for compatibility with other databases.
            md5digest digest;
            md5_deprecated(inputBytes.data(), inputBytes.length(), digest);
            return Value(BSONBinData(digest, sizeof(digest), BinDataGeneral));
        }
        case HashAlgorithm::sha256: {
            SHA256Block hash = SHA256Block::computeHash({inputBytes});
            return Value(BSONBinData(hash.data(), hash.size(), BinDataGeneral));
        }
        case HashAlgorithm::xxh64: {
            // We use the canonical (big endian) form for a platform-independent representation.
            XXH64_hash_t hash = ZSTD_XXH64(inputBytes.data(), inputBytes.length(), 0);
            XXH64_canonical_t canonical;
            ZSTD_XXH64_canonicalFromHash(&canonical, hash);
            return Value(BSONBinData(canonical.digest, sizeof(canonical.digest), BinDataGeneral));
        }
    }

    MONGO_UNREACHABLE;
}

}  // namespace exec::expression
}  // namespace mongo
