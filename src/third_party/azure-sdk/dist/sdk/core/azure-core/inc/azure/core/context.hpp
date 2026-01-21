// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Context for canceling long running operations.
 */

#pragma once

#include "azure/core/azure_assert.hpp"
#include "azure/core/datetime.hpp"
#include "azure/core/rtti.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

// Forward declare TracerProvider to resolve an include file dependency ordering problem.
namespace Azure { namespace Core { namespace Tracing {
  class TracerProvider;
}}} // namespace Azure::Core::Tracing

namespace Azure { namespace Core {

  /**
   * @brief An exception thrown when an operation is cancelled.
   */
  class OperationCancelledException final : public std::runtime_error {
  public:
    /**
     * @brief Constructs an `OperationCancelledException` with message string as the description.
     *
     * @param what The explanatory string.
     */
    explicit OperationCancelledException(std::string const& what) : std::runtime_error(what) {}
  };

  /**
   * @brief A context is a node within a unidirectional tree that represents deadlines and key/value
   * pairs.
   *
   * Most Azure Service Client operations accept a Context object. The Context object allows the
   * caller to cancel the operation if it is taking too long, or to ensure that the operation
   * completes before a specific deadline. This allows an application to apply timeouts to
   * operations, or to cancel operations that need to be abandoned.
   *
   * After cancelling a Context, all service operations which have the cancelled context
   * as a parent context will be cancelled. Cancellation is indicated by throwing an
   * Azure::Core::OperationCancelledException from the operation.
   *
   * Context objects support the following operations to create new contexts:
   * - WithDeadline(DateTime): creates a new child context with a deadline.
   * - WithValue(Key, T): creates a new child context with a key/value pair.
   *
   * Context objects support the following operations to retrieve data:
   * - GetDeadline(): gets the deadline for the context.
   * - TryGetValue(Key, T): gets the value associated with a key.
   *
   * Context objects support two operations to manage the context:
   * - Cancel(): cancels the context.
   * - IsCancelled(): checks if the context is cancelled.
   *
   * Context objects support the following operation to throw if the context is cancelled:
   * - ThrowIfCancelled(): throws an OperationCancelledException if the context is cancelled.
   *
   *
   */
  class Context final {
  public:
    /**
     * @brief A key used to store and retrieve data in an #Azure::Core::Context object.
     */
    class Key final {
      Key const* m_uniqueAddress;

    public:
      /**
       * @brief Constructs a default instance of `%Key`.
       *
       */
      Key() : m_uniqueAddress(this) {}

      /**
       * @brief Compares with \p other `%Key` for equality.
       * @param other Other `%Key` to compare with.
       * @return `true` if instances are equal; otherwise, `false`.
       */
      bool operator==(Key const& other) const
      {
        return this->m_uniqueAddress == other.m_uniqueAddress;
      }

      /**
       * @brief Compares with \p other `%Key` for equality.
       * @param other Other `%Key` to compare with.
       * @return `false` if instances are equal; otherwise, `true`.
       */
      bool operator!=(Key const& other) const { return !(*this == other); }
    };

  private:
    struct ContextSharedState final
    {
      std::shared_ptr<ContextSharedState> Parent;
      std::atomic<DateTime::rep> Deadline;
      std::shared_ptr<Azure::Core::Tracing::TracerProvider> TraceProvider;
      Context::Key Key;
      std::shared_ptr<void> Value;
#if defined(AZ_CORE_RTTI)
      const std::type_info& ValueType;
#endif
      static constexpr DateTime::rep ToDateTimeRepresentation(DateTime const& dateTime)
      {
        return dateTime.time_since_epoch().count();
      }

      static constexpr DateTime FromDateTimeRepresentation(DateTime::rep dtRepresentation)
      {
        return DateTime(DateTime::time_point(DateTime::duration(dtRepresentation)));
      }

      ContextSharedState(ContextSharedState const&) = delete;
      ContextSharedState(ContextSharedState&&) = delete;
      ContextSharedState& operator=(ContextSharedState const&) = delete;
      ContextSharedState&& operator=(ContextSharedState&&) = delete;

      /**
       * @brief Creates a new ContextSharedState object with no deadline and no value.
       */
      explicit ContextSharedState()
          : Deadline(ToDateTimeRepresentation((DateTime::max)())), Value(nullptr)
#if defined(AZ_CORE_RTTI)
            ,
            ValueType(typeid(std::nullptr_t))
#endif
      {
      }

      /**
       * @brief Create a new ContextSharedState from another ContextSharedState with a deadline.
       *
       * @param parent The parent context to create a child context from.
       * @param deadline The deadline for the new context.
       *
       */
      explicit ContextSharedState(
          const std::shared_ptr<ContextSharedState>& parent,
          DateTime const& deadline = (DateTime::max)())
          : Parent(parent), Deadline(ToDateTimeRepresentation(deadline)), Value(nullptr)
#if defined(AZ_CORE_RTTI)
            ,
            ValueType(typeid(std::nullptr_t))
#endif
      {
      }

      /**
       * @brief Create a new ContextSharedState from another ContextSharedState with a deadline and
       * a key value pair.
       *
       * @tparam T The type of the value to be stored with the key.
       * @param parent The parent context to create a child context from.
       * @param deadline The deadline for the new context.
       * @param key The key to associate with the value.
       * @param value The value to associate with the key.
       *
       */
      template <class T>
      explicit ContextSharedState(
          const std::shared_ptr<ContextSharedState>& parent,
          DateTime const& deadline,
          Context::Key const& key,
          T value) // NOTE, should this be T&&
          : Parent(parent), Deadline(ToDateTimeRepresentation(deadline)), Key(key),
            Value(std::make_shared<T>(std::move(value)))
#if defined(AZ_CORE_RTTI)
            ,
            ValueType(typeid(T))
#endif
      {
      }
    };

    std::shared_ptr<ContextSharedState> m_contextSharedState;

    explicit Context(std::shared_ptr<ContextSharedState> impl)
        : m_contextSharedState(std::move(impl))
    {
    }

  public:
    /**
     * @brief Constructs a context with no deadline, and no value associated.
     *
     */
    Context() : m_contextSharedState(std::make_shared<ContextSharedState>()) {}

    /**
     * @brief Constructs a context with a deadline
     * object.
     *
     * @param deadline A point in time after which a context expires.
     *
     */
    explicit Context(DateTime const& deadline)
        : m_contextSharedState(std::make_shared<ContextSharedState>(nullptr, deadline))
    {
    }

    /**
     * @brief Copies a context.
     *
     * This operation copies one context to another. Context objects are copied by reference,
     * so the new context will share the same state as the original context. This also means
     * that cancelling one context cancels all contexts which are copied from the original
     * context.
     *
     * 	@param other Another context to copy.
     *
     */
    Context(Context const&) = default;

    /**
     * @brief Assigns a context.
     *
     * This operation assigns one context to another. Context objects are copied by reference, so
     * the new context will share the same state as the original context. This also means that
     * cancelling one context cancels all contexts which are copied from the original context.
     *
     * @param other Another context to assign.
     *
     * @return A new Context referencing the state of the original context.
     */
    Context& operator=(Context const& other) = default;

    /**
     * @brief Moves a context.
     *
     * This operation moves one context to another.
     *
     * @param other The context to move.
     *
     */
    Context(Context&& other) = default;

    /**
     * @brief Moves a context.
     *
     * This operation moves one context to another.
     *
     * @param other The context to move.
     * @return A new Context referencing the state of the original context.
     *
     */
    Context& operator=(Context&& other) = default;

    /**
     * @brief Destroys a context.
     *
     */
    ~Context() = default;

    /**
     * @brief Creates a context with a deadline from an existing Context object.
     *
     * @param deadline A point in time after which a context expires.
     *
     * @return A child context with deadline.
     */
    Context WithDeadline(DateTime const& deadline) const
    {
      return Context{std::make_shared<ContextSharedState>(m_contextSharedState, deadline)};
    }

    /**
     * @brief Creates a new child context with \p key and \p value
     * associated with it.
     *
     * @tparam T The type of the value to be stored with the key.
     * @param key A key to associate with this context.
     * @param value A value to associate with this context.
     *
     * @return A child context with the \p key and \p value associated with it.
     */
    template <class T> Context WithValue(Key const& key, T&& value) const
    {
      return Context{std::make_shared<ContextSharedState>(
          m_contextSharedState, (DateTime::max)(), key, std::forward<T>(value))};
    }

    /**
     * @brief Gets the deadline for this context or the branch of contexts this context
     * belongs to.
     *
     * @return The deadline associated with the context; `Azure::DateTime::max()` if no deadline is
     * specified.
     *
     */
    DateTime GetDeadline() const;

    /**
     * @brief Gets the value associated with a \p key parameter within this context or the
     * branch of contexts this context belongs to.
     *
     * @tparam T The type of the value to be retrieved.
     * @param key A key associated with a context to find.
     * @param outputValue A reference to the value corresponding to the \p key to be set, if found
     * within the context tree.
     *
     * @return `true` if \p key is found, with \p outputValue set to the value associated with the
     * key found; otherwise, `false`.
     *
     * @note The \p outputValue is left unmodified if the \p key is not found.
     */
    template <class T> bool TryGetValue(Key const& key, T& outputValue) const
    {
      for (std::shared_ptr<ContextSharedState> ptr = m_contextSharedState; ptr; ptr = ptr->Parent)
      {
        if (ptr->Key == key)
        {
#if defined(AZ_CORE_RTTI)
          AZURE_ASSERT_MSG(
              typeid(T) == ptr->ValueType, "Type mismatch for Context::TryGetValue().");
#endif

          outputValue = *reinterpret_cast<const T*>(ptr->Value.get());
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Cancels the context. All operations which share this Context will be cancelled.
     *
     * @note Cancellation of a Context is a best-faith effort. Because of the synchronous nature of
     * Azure C++ SDK APIs, it is possible that the operation will not be cancelled immediately. Each
     * operation explicitly checks the context's state to determine if it has been cancelled,
     * those checks may not happen immediately, or at all.
     *
     * @note Once a context has been cancelled, the cancellation cannot be undone.
     *
     */
    void Cancel()
    {
      m_contextSharedState->Deadline
          = ContextSharedState::ToDateTimeRepresentation((DateTime::min)());
    }

    /**
     * @brief Checks if the context is cancelled.
     * @return `true` if this context is cancelled; otherwise, `false`.
     */
    bool IsCancelled() const { return GetDeadline() < std::chrono::system_clock::now(); }

    /** @brief Throws if the context is cancelled.
     *
     * @throw #Azure::Core::OperationCancelledException if the context is cancelled.
     */
    void ThrowIfCancelled() const
    {
      if (IsCancelled())
      {
        throw OperationCancelledException("Request was cancelled by context.");
      }
    }

    /** @brief The `ApplicationContext` is a deprecated singleton Context object.
     *
     * @note: The `ApplicationContext` object is deprecated and will be removed in a future release.
     * If your application is using `ApplicationContext`, you should create your own root context
     * and use it where you would have otherwise used `ApplicationContext`.
     *
     */
    [[deprecated(
        "ApplicationContext is no longer supported. Instead customers should create their "
        "own root context objects.")]] static const AZ_CORE_DLLEXPORT Context ApplicationContext;
  };
}} // namespace Azure::Core
