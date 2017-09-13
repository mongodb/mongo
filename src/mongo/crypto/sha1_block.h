/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/crypto/sha_block.h"

#include "mongo/util/make_array_type.h"

namespace mongo {

/**
 * A Traits type for adapting SHABlock to sha256 hashes.
 */
struct SHA1BlockTraits {
    using HashType = MakeArrayType<std::uint8_t, 20, SHA1BlockTraits>;

    static constexpr StringData name = "SHA1Block"_sd;

    static HashType computeHash(std::initializer_list<ConstDataRange> input);

    static void computeHmac(const uint8_t* key,
                            size_t keyLen,
                            const uint8_t* input,
                            size_t inputLen,
                            HashType* const output);
};

using SHA1Block = SHABlock<SHA1BlockTraits>;

}  // namespace mongo
