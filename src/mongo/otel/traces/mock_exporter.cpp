// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
