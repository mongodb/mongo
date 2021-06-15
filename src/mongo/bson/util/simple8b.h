/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include <cmath>
#include <iostream>
#include <vector>


namespace mongo {

/**
 * As of now, Simple8b is a static class that can encode a series of integers and a selector value
 * into a single 64 bit Simple8b word.
 */
class Simple8b {
public:
    // A valid Simple8b should never have a selector value of 1, so we will use it as an error code.
    static const uint64_t errCode = 0x1000000000000000;

    /**
     * encodeSimple8b takes a selector and a vector of integers to be compressed into a 64 bit word.
     * The values will be stored from left to right.
     * If there are wasted bits, they will be placed at the very end on the right.
     * For now, we will assume that all ints in the vector are greater or equal to zero.
     * We will also assume that the selector and all values will fit into the 64 bit word.
     * Returns the encoded Simple8b word if the inputs are valid and errCode otherwise.
     */
    static uint64_t encodeSimple8b(uint8_t selector, const std::vector<uint64_t>& values);

    /**
     * decodeSimple8b decodes a simple8b word into a vector of integers.
     * Only when the selector is invalid will the returned vector be empty.
     */
    static std::vector<uint64_t> decodeSimple8b(uint64_t simple8bWord);
};

}  // namespace mongo
