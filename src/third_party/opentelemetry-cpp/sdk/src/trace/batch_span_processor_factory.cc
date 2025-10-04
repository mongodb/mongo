// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <utility>

#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/batch_span_processor_factory.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
std::unique_ptr<SpanProcessor> BatchSpanProcessorFactory::Create(
    std::unique_ptr<SpanExporter> &&exporter,
    const BatchSpanProcessorOptions &options)
{
  std::unique_ptr<SpanProcessor> processor(new BatchSpanProcessor(std::move(exporter), options));
  return processor;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
