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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <ostream>

namespace mongo::sbe::value {

template <typename T>
class ValuePrinter;

/**
 * Companion static class to ValuePrinter template.
 */
class ValuePrinters {
    ValuePrinters() = delete;
    ValuePrinters(const ValuePrinters&) = delete;

public:
    static ValuePrinter<std::ostream> make(std::ostream& stream, const PrintOptions& options);
    static ValuePrinter<str::stream> make(str::stream& stream, const PrintOptions& options);
};

/**
 * Utility for printing values to stream.
 */
template <typename T>
class ValuePrinter {
    ValuePrinter() = delete;
    ValuePrinter(T& stream, const PrintOptions& options);
    friend class ValuePrinters;

public:
    void writeTagToStream(TypeTags tag);
    void writeStringDataToStream(StringData sd, bool addQuotes = true);
    void writeArrayToStream(TypeTags tag, Value val, size_t depth = 1);
    void writeSortedArraySetToStream(TypeTags tag, Value val, size_t depth = 1);
    void writeObjectToStream(TypeTags tag, Value val, size_t depth = 1);
    void writeMultiMapToStream(TypeTags tag, Value val, size_t depth = 1);
    void writeObjectToStream(const BSONObj& obj);
    void writeObjectIdToStream(TypeTags tag, Value val);
    void writeCollatorToStream(const CollatorInterface* collator);
    void writeBsonRegexToStream(const BsonRegex& regex);
    void writeNormalizedDouble(double value);
    void writeValueToStream(TypeTags tag, Value val, size_t depth = 1);

public:
    T& stream;
    PrintOptions options;
};

}  // namespace mongo::sbe::value
