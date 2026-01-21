// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/detail/token_cache.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>

using Azure::Identity::_detail::TokenCache;

using Azure::DateTime;
using Azure::Core::Credentials::AccessToken;

bool TokenCache::IsFresh(
    std::shared_ptr<TokenCache::CacheValue> const& item,
    DateTime::duration minimumExpiration,
    std::chrono::system_clock::time_point now)
{
  return item->AccessToken.ExpiresOn > (DateTime(now) + minimumExpiration);
}

namespace {
template <typename T> bool ShouldCleanUpCacheFromExpiredItems(T cacheSize);
}

std::shared_ptr<TokenCache::CacheValue> TokenCache::GetOrCreateValue(
    CacheKey const& key,
    DateTime::duration minimumExpiration) const
{
  {
    std::shared_lock<std::shared_timed_mutex> cacheReadLock(m_cacheMutex);

    auto const found = m_cache.find(key);
    if (found != TokenCache::m_cache.end())
    {
      return found->second;
    }
  }

#if defined(_azure_TESTING_BUILD)
  OnBeforeCacheWriteLock();
#endif

  std::unique_lock<std::shared_timed_mutex> cacheWriteLock(m_cacheMutex);

  // Search cache for the second time, in case the item was inserted between releasing the read lock
  // and acquiring the write lock.
  auto const found = m_cache.find(key);
  if (found != m_cache.end())
  {
    return found->second;
  }

  // Clean up cache from expired items.
  if (ShouldCleanUpCacheFromExpiredItems(m_cache.size()))
  {
    auto now = std::chrono::system_clock::now();

    auto iter = m_cache.begin();
    while (iter != m_cache.end())
    {
      // Should we end up erasing the element, iterator to current will become invalid, after
      // which we can't increment it. So we copy current, and safely advance the loop iterator.
      auto const curr = iter;
      ++iter;

      // We will try to obtain a write lock, but in a non-blocking way. We only lock it if no one
      // was holding it for read and write at a time. If it's busy in any way, we don't wait, but
      // move on.
      auto const item = curr->second;
      {
        std::unique_lock<std::shared_timed_mutex> lock(item->ElementMutex, std::defer_lock);
        if (lock.try_lock() && !IsFresh(item, minimumExpiration, now))
        {
          m_cache.erase(curr);
        }
      }
    }
  }

  // Insert the blank value value and return it.
  return m_cache[key] = std::make_shared<CacheValue>();
}

AccessToken TokenCache::GetToken(
    std::string const& scopeString,
    std::string const& tenantId,
    DateTime::duration minimumExpiration,
    std::function<AccessToken()> const& getNewToken) const
{
  auto const item = GetOrCreateValue({scopeString, tenantId}, minimumExpiration);

  {
    std::shared_lock<std::shared_timed_mutex> itemReadLock(item->ElementMutex);

    if (IsFresh(item, minimumExpiration, std::chrono::system_clock::now()))
    {
      return item->AccessToken;
    }
  }

#if defined(_azure_TESTING_BUILD)
  OnBeforeItemWriteLock();
#endif

  std::unique_lock<std::shared_timed_mutex> itemWriteLock(item->ElementMutex);

  // Check the expiration for the second time, in case it just got updated, after releasing the
  // itemReadLock, and before acquiring itemWriteLock.
  if (IsFresh(item, minimumExpiration, std::chrono::system_clock::now()))
  {
    return item->AccessToken;
  }

  auto const newToken = getNewToken();
  item->AccessToken = newToken;
  return newToken;
}

namespace {

// Compile-time Fibonacci sequence computation.
// Get() produces a std::array<T> containing the numbers in ascending order.
template <
    typename T, // Type
    T L = 0, // Left hand side
    T R = 1, // Right hand side
    size_t N = 0, // Counter (for array)
    bool X
    = (((std::numeric_limits<T>::max)() - L) < R)> // Condition to stop (integer overflow of T)
struct SortedFibonacciSequence
{
  static constexpr auto Get();
};

template <typename T, T L, T R, size_t N> struct SortedFibonacciSequence<T, L, R, N, true>
{
  static constexpr auto Get()
  {
    std::array<T, N + 1> result{};
    result[N] = L;
    return result;
  }
};

template <typename T, T L, T R, size_t N, bool X>
constexpr auto SortedFibonacciSequence<T, L, R, N, X>::Get()
{
  auto result = SortedFibonacciSequence<T, R, R + L, N + 1>::Get();
  result[N] = L;
  return result;
}

template <typename T> bool ShouldCleanUpCacheFromExpiredItems(T cacheSize)
{
  static auto const Fibonacci = SortedFibonacciSequence<T, 1, 2>::Get();
  return std::binary_search(Fibonacci.begin(), Fibonacci.end(), cacheSize);
}

} // namespace
