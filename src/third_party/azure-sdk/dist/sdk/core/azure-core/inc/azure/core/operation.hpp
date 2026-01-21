// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Provides support for long-running operations.
 */

#pragma once

#include "azure/core/context.hpp"
#include "azure/core/operation_status.hpp"
#include "azure/core/response.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace Azure { namespace Core {

  /**
   * @brief Methods starting long-running operations return Operation<T> types.
   *
   * @tparam T The long-running operation final result type.
   */
  template <class T> class Operation {
  private:
    // These are pure virtual b/c the derived class must provide an implementation
    virtual std::unique_ptr<Http::RawResponse> PollInternal(Context const& context) = 0;
    virtual Response<T> PollUntilDoneInternal(std::chrono::milliseconds period, Context& context)
        = 0;

    [[deprecated("Do not override and do not use.")]] virtual Azure::Core::Http::RawResponse const&
    GetRawResponseInternal() const
    {
      return *m_rawResponse;
    }

  protected:
    /** @brief the underlying raw response for this operation. */
    std::unique_ptr<Azure::Core::Http::RawResponse> m_rawResponse = nullptr;

    /** @brief the current status of the operation. */
    OperationStatus m_status = OperationStatus::NotStarted;

    /**
     * @brief Constructs a default instance of `%Operation`.
     *
     */
    Operation() = default;

    // Define how an Operation<T> can be move-constructed from rvalue other. Parameter `other`
    // gave up ownership for the rawResponse.
    /**
     * @brief Constructs an instance of `%Operation` by moving in another instance.
     *
     * @param other An `%Operation` instance to move in.
     */
    Operation(Operation&& other)
        : m_rawResponse(std::move(other.m_rawResponse)), m_status(other.m_status)
    {
    }

    // Define how an Operation<T> can be copy-constructed from some other Operation reference.
    // Operation will create a clone of the rawResponse from `other`.
    /**
     * @brief Constructs an instance of `%Operation` by copying another instance.
     *
     * @param other An `%Operation` instance to copy.
     */
    Operation(Operation const& other)
        : m_rawResponse(std::make_unique<Http::RawResponse>(other.GetRawResponse())),
          m_status(other.m_status)
    {
    }

    // Define how an Operation<T> can be move-assigned from rvalue other. Parameter `other`
    // gave up ownership for the rawResponse.
    /**
     * @brief Assigns an instance of `%Operation` by moving in another instance.
     *
     * @param other An `%Operation` instance to move in.
     *
     * @return A reference to this instance.
     */
    Operation& operator=(Operation&& other)
    {
      this->m_rawResponse = std::move(other.m_rawResponse);
      this->m_status = other.m_status;
      return *this;
    }

    // Define how an Operation<T> can be copy-assigned from some other Operation reference.
    // Operation will create a clone of the rawResponse from `other`.
    /**
     * @brief Assigns another `%Operation` instance by copying.
     *
     * @param other An `%Operation` instance to copy.
     *
     * @return A reference to this instance.
     */
    Operation& operator=(Operation const& other)
    {
      this->m_rawResponse = std::make_unique<Http::RawResponse>(other.GetRawResponse());
      this->m_status = other.m_status;
      return *this;
    }

  public:
    /**
     * @brief Destructs the `%Operation`.
     *
     */
    virtual ~Operation() {}

    /**
     * @brief Final result of the long-running operation.
     *
     * @return The final result of the long-running operation.
     */
    virtual T Value() const = 0;

    /**
     * @brief Gets a token representing the operation that can be used to poll for the status of
     * the long-running operation.
     *
     * @return std::string The resume token.
     */
    virtual std::string GetResumeToken() const = 0;

    /**
     * @brief Gets the raw HTTP response.
     * @return A reference to an #Azure::Core::Http::RawResponse.
     * @note Does not give up ownership of the RawResponse.
     */
    Azure::Core::Http::RawResponse const& GetRawResponse() const
    {
      if (!m_rawResponse)
      {
        throw std::runtime_error("The raw response was not yet set for the Operation.");
      }
      return *m_rawResponse;
    }

    /**
     * @brief Gets the current #Azure::Core::OperationStatus of the long-running operation.
     *
     */
    OperationStatus const& Status() const noexcept { return m_status; }

    /**
     * @brief Checks if the long-running operation is completed.
     *
     * @return `true` if the long-running operation is done; otherwise, `false`.
     */
    bool IsDone() const noexcept
    {
      return (
          m_status == OperationStatus::Succeeded || m_status == OperationStatus::Cancelled
          || m_status == OperationStatus::Failed);
    }

    /**
     * @brief Checks if the long-running operation completed successfully and has produced a
     * final result.
     * @note The final result is accessible from `Value()`.
     *
     * @return `true` if the long-running operation completed successfully; otherwise, `false`.
     */
    bool HasValue() const noexcept { return (m_status == OperationStatus::Succeeded); }

    /**
     * @brief Gets updated status of the long-running operation.
     *
     * @return An HTTP #Azure::Core::Http::RawResponse returned from the service.
     */
    Http::RawResponse const& Poll()
    {
      // In the cases where the customer doesn't want to use a context we new one up and pass it
      // through
      return Poll(Context{});
    }

    /**
     * @brief Gets updated status of the long-running operation.
     *
     * @param context A context to control the request lifetime.
     *
     * @return An HTTP #Azure::Core::Http::RawResponse returned from the service.
     */
    Http::RawResponse const& Poll(Context const& context)
    {
      context.ThrowIfCancelled();
      m_rawResponse = PollInternal(context);
      return *m_rawResponse;
    }

    /**
     * @brief Periodically polls till the long-running operation completes.
     *
     * @param period Time in milliseconds to wait between polls.
     *
     * @return Response<T> the final result of the long-running operation.
     */
    Response<T> PollUntilDone(std::chrono::milliseconds period)
    {
      // In the cases where the customer doesn't want to use a context we new one up and pass it
      // through
      Context context;
      return PollUntilDone(period, context);
    }

    /**
     * @brief Periodically polls till the long-running operation completes;
     *
     * @param period Time in milliseconds to wait between polls.
     * @param context A context to control the request lifetime.
     *
     * @return Response<T> the final result of the long-running operation.
     */
    Response<T> PollUntilDone(std::chrono::milliseconds period, Context& context)
    {
      context.ThrowIfCancelled();
      return PollUntilDoneInternal(period, context);
    }
  };
}} // namespace Azure::Core
