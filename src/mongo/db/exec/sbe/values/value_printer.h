// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <ostream>
#include <string_view>

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
    void writeStringDataToStream(std::string_view sd, bool addQuotes = true);
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
