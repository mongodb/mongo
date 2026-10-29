// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/stdx/unordered_set.h"

#include <string_view>

#include <opentelemetry/context/propagation/text_map_propagator.h>

namespace mongo {
namespace otel {

using OtelStringView = opentelemetry::nostd::string_view;
using TextMapCarrier = opentelemetry::context::propagation::TextMapCarrier;

OtelStringView asOtelStringView(std::string_view data);
std::string_view asStringData(OtelStringView view);

stdx::unordered_set<OtelStringView> getKeySet(const TextMapCarrier& carrier);

}  // namespace otel
}  // namespace mongo
