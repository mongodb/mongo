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

#include "mongo/otel/traces/mock_exporter.h"

#include "mongo/config.h"

#include <string_view>

namespace mongo {
namespace otel {
namespace traces {

using opentelemetry::sdk::trace::Recordable;

void MockRecordable::SetAttribute(std::string_view key,
                                  const opentelemetry::common::AttributeValue& value) noexcept {
    // Copy string-type attribute values into _ownedStrings so that the string_view entries
    // stored in `attributes` remain valid for the lifetime of this recordable.
    // TODO(SERVER-129450): Other attribute types may need to be stored here.
    auto stableValue = std::visit(
        [this](auto&& v) -> opentelemetry::common::AttributeValue {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string_view>) {
                _ownedStrings.emplace_back(v.data(), v.size());
                return std::string_view{_ownedStrings.back()};
            } else if constexpr (std::is_same_v<T, const char*>) {
                _ownedStrings.emplace_back(v);
                return std::string_view{_ownedStrings.back()};
            } else {
                return v;
            }
        },
        value);
    attributes[std::string{key}] = std::move(stableValue);
}

std::unique_ptr<Recordable> MockExporter::MakeRecordable() noexcept {
    return std::make_unique<MockRecordable>();
}

opentelemetry::sdk::common::ExportResult MockExporter::Export(
    const opentelemetry::nostd::span<std::unique_ptr<Recordable>>& spans) noexcept {
    for (auto& span : spans) {
        MockRecordable* mock = dynamic_cast<MockRecordable*>(span.get());
        if (mock != nullptr) {
            std::ignore = span.release();
            _exportedSpans.push_back(std::unique_ptr<MockRecordable>(mock));
        }
    }

    return opentelemetry::sdk::common::ExportResult::kSuccess;
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo
