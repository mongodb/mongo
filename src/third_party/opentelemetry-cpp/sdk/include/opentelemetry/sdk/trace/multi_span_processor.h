// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include "opentelemetry/sdk/trace/multi_recordable.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/** Instantiation options. */
struct MultiSpanProcessorOptions
{};

/**
 * Span processor allow hooks for span start and end method invocations.
 *
 * Built-in span processors are responsible for batching and conversion of
 * spans to exportable representation and passing batches to exporters.
 */
class MultiSpanProcessor : public SpanProcessor
{
public:
  MultiSpanProcessor(std::vector<std::unique_ptr<SpanProcessor>> &&processors)
      : head_(nullptr), tail_(nullptr), count_(0)
  {
    for (auto &processor : processors)
    {
      AddProcessor(std::move(processor));
    }
  }

  void AddProcessor(std::unique_ptr<SpanProcessor> &&processor)
  {
    // Add preocessor to end of the list.
    if (processor)
    {
      ProcessorNode *pNode = new ProcessorNode(std::move(processor), tail_);
      if (count_ > 0)
      {
        tail_->next_ = pNode;
        tail_        = pNode;
      }
      else
      {
        head_ = tail_ = pNode;
      }
      count_++;
    }
  }

  std::unique_ptr<Recordable> MakeRecordable() noexcept override
  {
    auto recordable       = std::unique_ptr<Recordable>(new MultiRecordable);
    auto multi_recordable = static_cast<MultiRecordable *>(recordable.get());
    ProcessorNode *node   = head_;
    while (node != nullptr)
    {
      auto processor = node->value_.get();
      multi_recordable->AddRecordable(*processor, processor->MakeRecordable());
      node = node->next_;
    }
    return recordable;
  }

  virtual void OnStart(Recordable &span,
                       const opentelemetry::trace::SpanContext &parent_context) noexcept override
  {
    auto multi_recordable = static_cast<MultiRecordable *>(&span);
    ProcessorNode *node   = head_;
    while (node != nullptr)
    {
      auto processor   = node->value_.get();
      auto &recordable = multi_recordable->GetRecordable(*processor);
      if (recordable != nullptr)
      {
        processor->OnStart(*recordable, parent_context);
      }
      node = node->next_;
    }
  }

  virtual void OnEnd(std::unique_ptr<Recordable> &&span) noexcept override
  {
    auto multi_recordable = static_cast<MultiRecordable *>(span.release());
    ProcessorNode *node   = head_;
    while (node != nullptr)
    {
      auto processor  = node->value_.get();
      auto recordable = multi_recordable->ReleaseRecordable(*processor);
      if (recordable != nullptr)
      {
        processor->OnEnd(std::move(recordable));
      }
      node = node->next_;
    }
    delete multi_recordable;
  }

  bool ForceFlush(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override
  {
    bool result         = true;
    ProcessorNode *node = head_;
    while (node != nullptr)
    {
      auto processor = node->value_.get();
      result |= processor->ForceFlush(timeout);
      node = node->next_;
    }
    return result;
  }

  bool Shutdown(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override
  {
    bool result         = true;
    ProcessorNode *node = head_;
    while (node != nullptr)
    {
      auto processor = node->value_.get();
      result |= processor->Shutdown(timeout);
      node = node->next_;
    }
    return result;
  }

  ~MultiSpanProcessor() override
  {
    Shutdown();
    Cleanup();
  }

private:
  struct ProcessorNode
  {
    std::unique_ptr<SpanProcessor> value_;
    ProcessorNode *next_, *prev_;
    ProcessorNode(std::unique_ptr<SpanProcessor> &&value,
                  ProcessorNode *prev = nullptr,
                  ProcessorNode *next = nullptr)
        : value_(std::move(value)), next_(next), prev_(prev)
    {}
  };

  void Cleanup()
  {
    if (count_)
    {
      ProcessorNode *node = tail_;
      while (node != nullptr)
      {
        if (node->next_ != nullptr)
        {
          delete node->next_;
          node->next_ = nullptr;
        }
        if (node->prev_ != nullptr)
        {
          node = node->prev_;
        }
        else
        {
          delete node;
          node = nullptr;
        }
      }
      head_ = tail_ = nullptr;
      count_        = 0;
    }
  }

  ProcessorNode *head_, *tail_;
  size_t count_;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
