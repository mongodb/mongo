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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/simple8b.h"

#include <memory>

namespace mongo {

/**
 * Class to build BSON Subtype 7 (Column) binaries.
 */
class BSONColumnBuilder {
public:
    BSONColumnBuilder(StringData fieldName);

    /**
     * Appends a BSONElement to this BSONColumnBuilder.
     *
     * Value will be stored delta compressed if possible and uncompressed otherwise.
     *
     * The field name will be ignored.
     */
    BSONColumnBuilder& append(BSONElement elem);

    /**
     * Appends an index skip to this BSONColumnBuilder.
     */
    BSONColumnBuilder& skip();

    /**
     * Returns the field name this BSONColumnBuilder was created with.
     */
    StringData fieldName() const {
        return _fieldName;
    }

    /**
     * Finalizes the BSON Column and returns the BinData binary.
     *
     * The BSONColumnBuilder must remain in scope for the pointer to be valid.
     */
    BSONBinData finalize();

private:
    BSONElement _previous() const;

    void _storePrevious(BSONElement elem);
    void _writeLiteralFromPrevious();
    void _incrementSimple8bCount();
    bool _usesDeltaOfDelta(BSONType type);

    Simple8bBuilder<uint64_t> _createSimple8bBuilder();

    // Storage for the previously appended BSONElement
    std::unique_ptr<char[]> _prev;
    int _prevSize = 0;
    int _prevCapacity = 0;
    // This is only used for types that use delta of delta.
    int64_t _prevDelta = 0;

    // Simple-8b builder for storing compressed deltas
    Simple8bBuilder<uint64_t> _simple8bBuilder;

    // Offset to last Simple-8b control byte
    std::ptrdiff_t _controlByteOffset = 0;

    // Buffer for the BSON Column binary
    BufBuilder _bufBuilder;

    // Field name
    std::string _fieldName;
};

}  // namespace mongo
