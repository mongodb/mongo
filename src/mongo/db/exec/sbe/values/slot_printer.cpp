// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/slot_printer.h"

#include <cstddef>

namespace mongo::sbe::value {

template <typename T>
SlotPrinter<T>::SlotPrinter(T& stream, const PrintOptions& options)
    : _stream(stream), _options(options), _valuePrinter(ValuePrinters::make(stream, options)) {}

template <typename T>
void SlotPrinter<T>::printMaterializedRow(const MaterializedRow& row) {
    _stream << "[";
    for (std::size_t idx = 0; idx < row.size(); idx++) {
        if (idx > 0) {
            _stream << ", ";
        }
        auto [tag, val] = row.getViewOfValue(idx);
        _valuePrinter.writeValueToStream(tag, val);
    }
    _stream << "]";
}

template class SlotPrinter<std::ostream>;
template class SlotPrinter<str::stream>;

SlotPrinter<std::ostream> SlotPrinters::make(std::ostream& stream, const PrintOptions& options) {
    return SlotPrinter(stream, options);
}

SlotPrinter<str::stream> SlotPrinters::make(str::stream& stream, const PrintOptions& options) {
    return SlotPrinter(stream, options);
}

}  // namespace mongo::sbe::value
