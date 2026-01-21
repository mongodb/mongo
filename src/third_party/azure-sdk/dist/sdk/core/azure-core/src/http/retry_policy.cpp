// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/policies/policy.hpp"
#include "azure/core/internal/diagnostics/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <thread>

using Azure::Core::Context;
using namespace Azure::Core::Http;
using namespace Azure::Core::Http::Policies;
using namespace Azure::Core::Http::Policies::_internal;

namespace {
bool GetResponseHeaderBasedDelay(RawResponse const& response, std::chrono::milliseconds& retryAfter)
{
  // Try to find retry-after headers. There are several of them possible.
  auto const& responseHeaders = response.GetHeaders();
  auto const responseHeadersEnd = responseHeaders.end();
  auto header = responseHeadersEnd;
  if (((header = responseHeaders.find("retry-after-ms")) != responseHeadersEnd)
      || ((header = responseHeaders.find("x-ms-retry-after-ms")) != responseHeadersEnd))
  {
    // The headers above are in milliseconds.
    retryAfter = std::chrono::milliseconds(std::stoi(header->second));
    return true;
  }

  if ((header = responseHeaders.find("retry-after")) != responseHeadersEnd)
  {
    // This header is in seconds.
    retryAfter = std::chrono::seconds(std::stoi(header->second));
    return true;

    // Tracked by https://github.com/Azure/azure-sdk-for-cpp/issues/262
    // ----------------------------------------------------------------
    //
    // To be accurate, the Retry-After header is EITHER seconds, or a DateTime. So we need to
    // write a parser for that (and handle the case when parsing seconds fails).
    // More info:
    // * Retry-After header: https://developer.mozilla.org/docs/Web/HTTP/Headers/Retry-After
    // * HTTP Date format: https://developer.mozilla.org/docs/Web/HTTP/Headers/Date
    // * Parsing the date: https://en.cppreference.com/w/cpp/locale/time_get
    // * Get system datetime: https://en.cppreference.com/w/cpp/chrono/system_clock/now
    // * Subtract datetimes to get duration:
    // https://en.cppreference.com/w/cpp/chrono/time_point/operator_arith2
  }

  return false;
}

/**
 * @brief Calculate the exponential delay needed for this retry.
 *
 * @param retryOptions Options controlling the delay algorithm.
 * @param attempt Which attempt is this?
 * @param jitterFactor Test hook removing the randomness from the delay algorithm.
 *
 * @returns Number of milliseconds to delay.
 *
 * @remarks This function calculates the exponential backoff needed for each retry, including a
 * jitter factor.
 */
std::chrono::milliseconds CalculateExponentialDelay(
    RetryOptions const& retryOptions,
    int32_t attempt,
    double jitterFactor)
{
  if (jitterFactor < 0.8 || jitterFactor > 1.3)
  {
    // jitterFactor is a random double number in the range [0.8 .. 1.3]
    jitterFactor
        = 0.8 + ((static_cast<double>(static_cast<int32_t>(std::rand())) / RAND_MAX) * 0.5);
  }

  constexpr auto beforeLastBit
      = std::numeric_limits<int32_t>::digits - (std::numeric_limits<int32_t>::is_signed ? 1 : 0);

  // Scale exponentially: 1 x RetryDelay on 1st attempt, 2x on 2nd, 4x on 3rd, 8x on 4th ... all the
  // way up to (std::numeric_limits<int32_t>::max()) * RetryDelay.
  auto exponentialRetryAfter = retryOptions.RetryDelay
      * (((attempt - 1) <= beforeLastBit) ? (1 << (attempt - 1))
                                          : (std::numeric_limits<int32_t>::max()));

  // Multiply exponentialRetryAfter by jitterFactor
  exponentialRetryAfter = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
      (std::chrono::duration<double, std::chrono::milliseconds::period>(exponentialRetryAfter)
       * jitterFactor)
          .count()));

  return (std::min)(exponentialRetryAfter, retryOptions.MaxRetryDelay);
}

bool WasLastAttempt(RetryOptions const& retryOptions, int32_t attempt)
{
  return attempt > retryOptions.MaxRetries;
}

Context::Key const RetryKey;
} // namespace

int32_t RetryPolicy::GetRetryCount(Context const& context)
{
  int32_t number = -1;

  // Context with no data abut sending request with retry policy = -1
  // First try = 0
  // Second try = 1
  // third try = 2
  // ...
  int32_t* ptr = &number;
  context.TryGetValue<int32_t*>(RetryKey, ptr);

  return *ptr;
}

std::unique_ptr<RawResponse> RetryPolicy::Send(
    Request& request,
    NextHttpPolicy nextPolicy,
    Context const& context) const
{
  using Azure::Core::Diagnostics::Logger;
  using Azure::Core::Diagnostics::_internal::Log;
  // retryCount needs to be apart from RetryNumber attempt.
  int32_t retryCount = 0;
  auto retryContext = context.WithValue(RetryKey, &retryCount);

  for (int32_t attempt = 1;; ++attempt)
  {
    std::chrono::milliseconds retryAfter{};
    request.StartTry();
    // creates a copy of original query parameters from request
    auto originalQueryParameters = request.GetUrl().GetQueryParameters();

    try
    {
      auto response = nextPolicy.Send(request, retryContext);

      // If we are out of retry attempts, if a response is non-retriable (or simply 200 OK, i.e
      // doesn't need to be retried), then ShouldRetry returns false.
      if (!ShouldRetryOnResponse(*response.get(), m_retryOptions, attempt, retryAfter))
      {
        // If this is the second attempt and StartTry was called, we need to stop it. Otherwise
        // trying to perform same request would use last retry query/headers
        return response;
      }
    }
    catch (const TransportException& e)
    {
      if (Log::ShouldWrite(Logger::Level::Warning))
      {
        Log::Write(Logger::Level::Warning, std::string("HTTP Transport error: ") + e.what());
      }

      if (!ShouldRetryOnTransportFailure(m_retryOptions, attempt, retryAfter))
      {
        throw;
      }
    }

    if (Log::ShouldWrite(Logger::Level::Informational))
    {
      std::ostringstream log;

      log << "HTTP Retry attempt #" << attempt << " will be made in "
          << std::chrono::duration_cast<std::chrono::milliseconds>(retryAfter).count() << "ms.";

      Log::Write(Logger::Level::Informational, log.str());
    }

    // Sleep(0) behavior is implementation-defined: it may yield, or may do nothing. Let's make sure
    // we proceed immediately if it is 0.
    if (retryAfter.count() > 0)
    {
      // Before sleeping, check to make sure that the context hasn't already been cancelled.
      context.ThrowIfCancelled();
      std::this_thread::sleep_for(retryAfter);
    }

    // Restore the original query parameters before next retry
    request.GetUrl().SetQueryParameters(std::move(originalQueryParameters));

    // Update retry number
    retryCount += 1;
  }
}

bool RetryPolicy::ShouldRetryOnTransportFailure(
    RetryOptions const& retryOptions,
    int32_t attempt,
    std::chrono::milliseconds& retryAfter,
    double jitterFactor) const
{
  // Are we out of retry attempts?
  if (WasLastAttempt(retryOptions, attempt))
  {
    return false;
  }

  retryAfter = CalculateExponentialDelay(retryOptions, attempt, jitterFactor);
  return true;
}

bool RetryPolicy::ShouldRetryOnResponse(
    RawResponse const& response,
    RetryOptions const& retryOptions,
    int32_t attempt,
    std::chrono::milliseconds& retryAfter,
    double jitterFactor) const
{
  using Azure::Core::Diagnostics::Logger;
  using Azure::Core::Diagnostics::_internal::Log;

  // Are we out of retry attempts?
  if (WasLastAttempt(retryOptions, attempt))
  {
    return false;
  }

  // Should we retry on the given response retry code?
  {
    auto const& statusCodes = retryOptions.StatusCodes;
    auto const sc = response.GetStatusCode();
    if (statusCodes.find(sc) == statusCodes.end())
    {
      if (Log::ShouldWrite(Logger::Level::Informational))
      {
        Log::Write(
            Logger::Level::Informational,
            std::string("HTTP status code ") + std::to_string(static_cast<int>(sc))
                + " won't be retried.");
      }

      return false;
    }
    else if (Log::ShouldWrite(Logger::Level::Informational))
    {
      Log::Write(
          Logger::Level::Informational,
          std::string("HTTP status code ") + std::to_string(static_cast<int>(sc))
              + " will be retried.");
    }
  }

  if (!GetResponseHeaderBasedDelay(response, retryAfter))
  {
    retryAfter = CalculateExponentialDelay(retryOptions, attempt, jitterFactor);
  }

  return true;
}
