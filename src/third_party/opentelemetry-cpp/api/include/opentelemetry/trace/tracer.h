// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include "opentelemetry/context/context.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/trace/default_span.h"
#include "opentelemetry/trace/scope.h"
#include "opentelemetry/trace/span.h"
#include "opentelemetry/trace/span_context_kv_iterable_view.h"
#include "opentelemetry/trace/span_startoptions.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace trace
{
/**
 * Handles span creation and in-process context propagation.
 *
 * This class provides methods for manipulating the context, creating spans, and controlling spans'
 * lifecycles.
 */
class Tracer
{
public:
  virtual ~Tracer() = default;
  /**
   * Starts a span.
   *
   * Optionally sets attributes at Span creation from the given key/value pairs.
   *
   * Attributes will be processed in order, previous attributes with the same
   * key will be overwritten.
   */
  virtual nostd::shared_ptr<Span> StartSpan(nostd::string_view name,
                                            const common::KeyValueIterable &attributes,
                                            const SpanContextKeyValueIterable &links,
                                            const StartSpanOptions &options = {}) noexcept = 0;

  nostd::shared_ptr<Span> StartSpan(nostd::string_view name,
                                    const StartSpanOptions &options = {}) noexcept
  {
    return this->StartSpan(name, {}, {}, options);
  }

  template <class T,
            nostd::enable_if_t<common::detail::is_key_value_iterable<T>::value> * = nullptr>
  nostd::shared_ptr<Span> StartSpan(nostd::string_view name,
                                    const T &attributes,
                                    const StartSpanOptions &options = {}) noexcept
  {
    return this->StartSpan(name, attributes, {}, options);
  }

  nostd::shared_ptr<Span> StartSpan(nostd::string_view name,
                                    const common::KeyValueIterable &attributes,
                                    const StartSpanOptions &options = {}) noexcept
  {
    return this->StartSpan(name, attributes, NullSpanContext(), options);
  }

  template <class T,
            class U,
            nostd::enable_if_t<common::detail::is_key_value_iterable<T>::value> * = nullptr,
            nostd::enable_if_t<detail::is_span_context_kv_iterable<U>::value>   * = nullptr>
  nostd::shared_ptr<Span> StartSpan(nostd::string_view name,
                                    const T &attributes,
                                    const U &links,
                                    const StartSpanOptions &options = {}) noexcept
  {
    return this->StartSpan(name, common::KeyValueIterableView<T>(attributes),
                           SpanContextKeyValueIterableView<U>(links), options);
  }

  nostd::shared_ptr<Span> StartSpan(
      nostd::string_view name,
      std::initializer_list<std::pair<nostd::string_view, common::AttributeValue>> attributes,
      const StartSpanOptions &options = {}) noexcept
  {

    return this->StartSpan(name, attributes, {}, options);
  }

  template <class T,
            nostd::enable_if_t<common::detail::is_key_value_iterable<T>::value> * = nullptr>
  nostd::shared_ptr<Span> StartSpan(
      nostd::string_view name,
      const T &attributes,
      std::initializer_list<
          std::pair<SpanContext,
                    std::initializer_list<std::pair<nostd::string_view, common::AttributeValue>>>>
          links,
      const StartSpanOptions &options = {}) noexcept
  {
    return this->StartSpan(
        name, attributes,
        nostd::span<const std::pair<SpanContext, std::initializer_list<std::pair<
                                                     nostd::string_view, common::AttributeValue>>>>{
            links.begin(), links.end()},
        options);
  }

  template <class T,
            nostd::enable_if_t<common::detail::is_key_value_iterable<T>::value> * = nullptr>
  nostd::shared_ptr<Span> StartSpan(
      nostd::string_view name,
      std::initializer_list<std::pair<nostd::string_view, common::AttributeValue>> attributes,
      const T &links,
      const StartSpanOptions &options = {}) noexcept
  {
    return this->StartSpan(name,
                           nostd::span<const std::pair<nostd::string_view, common::AttributeValue>>{
                               attributes.begin(), attributes.end()},
                           links, options);
  }

  nostd::shared_ptr<Span> StartSpan(
      nostd::string_view name,
      std::initializer_list<std::pair<nostd::string_view, common::AttributeValue>> attributes,
      std::initializer_list<
          std::pair<SpanContext,
                    std::initializer_list<std::pair<nostd::string_view, common::AttributeValue>>>>
          links,
      const StartSpanOptions &options = {}) noexcept
  {
    return this->StartSpan(
        name,
        nostd::span<const std::pair<nostd::string_view, common::AttributeValue>>{attributes.begin(),
                                                                                 attributes.end()},
        nostd::span<const std::pair<SpanContext, std::initializer_list<std::pair<
                                                     nostd::string_view, common::AttributeValue>>>>{
            links.begin(), links.end()},
        options);
  }

  /**
   * Set the active span. The span will remain active until the returned Scope
   * object is destroyed.
   * @param span the span that should be set as the new active span.
   * @return a Scope that controls how long the span will be active.
   */
  static Scope WithActiveSpan(nostd::shared_ptr<Span> &span) noexcept { return Scope{span}; }

  /**
   * Get the currently active span.
   * @return the currently active span, or an invalid default span if no span
   * is active.
   */
  static nostd::shared_ptr<Span> GetCurrentSpan() noexcept
  {
    context::ContextValue active_span = context::RuntimeContext::GetValue(kSpanKey);
    if (nostd::holds_alternative<nostd::shared_ptr<Span>>(active_span))
    {
      return nostd::get<nostd::shared_ptr<Span>>(active_span);
    }
    else
    {
      return nostd::shared_ptr<Span>(new DefaultSpan(SpanContext::GetInvalid()));
    }
  }

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
  /**
   * Reports if the tracer is enabled or not. A disabled tracer will not create spans.
   *
   * The instrumentation authors should call this method before creating a spans to
   * potentially avoid performing computationally expensive operations for disabled tracers.
   *
   * @since ABI_VERSION 2
   */
  bool Enabled() const noexcept { return OPENTELEMETRY_ATOMIC_READ_8(&this->enabled_) != 0; }
#endif

#if OPENTELEMETRY_ABI_VERSION_NO == 1

  /*
   * The following is removed from the API in ABI version 2.
   * It belongs to the SDK.
   */

  /**
   * Force any buffered spans to flush.
   * @param timeout to complete the flush
   */
  template <class Rep, class Period>
  void ForceFlush(std::chrono::duration<Rep, Period> timeout) noexcept
  {
    this->ForceFlushWithMicroseconds(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count()));
  }

  virtual void ForceFlushWithMicroseconds(uint64_t timeout) noexcept = 0;

  /**
   * ForceFlush any buffered spans and stop reporting spans.
   * @param timeout to complete the flush
   */
  template <class Rep, class Period>
  void Close(std::chrono::duration<Rep, Period> timeout) noexcept
  {
    this->CloseWithMicroseconds(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count()));
  }

  virtual void CloseWithMicroseconds(uint64_t timeout) noexcept = 0;

#endif /* OPENTELEMETRY_ABI_VERSION_NO */

protected:
#if OPENTELEMETRY_ABI_VERSION_NO >= 2

  /**
   * Updates the enabled state of the tracer. Calling this method will affect the result of the
   * subsequent calls to {@code opentelemetry::v2::trace::Tracer::Enabled()}.
   *
   * This method should be used by SDK implementations to indicate the tracer's updated state
   * whenever a tracer transitions from enabled to disabled state and vice versa.
   *
   * @param enabled The new state of the tracer. False would indicate that the tracer is no longer
   * enabled and will not produce as
   *
   * @since ABI_VERSION 2
   */
  void UpdateEnabled(const bool enabled) noexcept
  {
    OPENTELEMETRY_ATOMIC_WRITE_8(&this->enabled_, enabled);
  }
#endif

private:
#if OPENTELEMETRY_ABI_VERSION_NO >= 2
  // Variable to support implementation of Enabled method introduced in ABI V2.
  // Mutable allows enabled_ to be used as 'bool *' (instead of 'const bool *'), with the
  // OPENTELEMETRY_ATOMIC_READ_8 macro's internal casts when used from a const function.
  // std::atomic can not be used here because it is not ABI compatible for OpenTelemetry C++ API.
  mutable bool enabled_ = true;
#endif
};
}  // namespace trace
OPENTELEMETRY_END_NAMESPACE
