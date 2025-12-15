// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/memory/in_memory_span_exporter_factory.h"
#include "opentelemetry/exporters/memory/in_memory_span_data.h"
#include "opentelemetry/exporters/memory/in_memory_span_exporter.h"
#include "opentelemetry/sdk/trace/exporter.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace memory
{

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> InMemorySpanExporterFactory::Create(
    std::shared_ptr<InMemorySpanData> &data)
{
  return Create(data, MAX_BUFFER_SIZE);
}

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> InMemorySpanExporterFactory::Create(
    std::shared_ptr<InMemorySpanData> &data,
    size_t buffer_size)
{
  InMemorySpanExporter *memory_exporter = new InMemorySpanExporter(buffer_size);
  data                                  = memory_exporter->GetData();
  std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter(memory_exporter);
  return exporter;
}

}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
