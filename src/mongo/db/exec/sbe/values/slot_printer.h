// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <ostream>

namespace mongo::sbe::value {

template <typename T>
class SlotPrinter;

/**
 * Companion static class to SlotPrinter template.
 */
class SlotPrinters {
    SlotPrinters() = delete;
    SlotPrinters(const SlotPrinters&) = delete;

public:
    static SlotPrinter<std::ostream> make(std::ostream& stream, const PrintOptions& options);
    static SlotPrinter<str::stream> make(str::stream& stream, const PrintOptions& options);
};

template <typename T>
class SlotPrinter {
    SlotPrinter() = delete;
    SlotPrinter(T& stream, const PrintOptions& options);
    friend class SlotPrinters;

public:
    void printMaterializedRow(const MaterializedRow& row);

private:
    T& _stream;
    const PrintOptions& _options;
    value::ValuePrinter<T> _valuePrinter;
};

}  // namespace mongo::sbe::value
