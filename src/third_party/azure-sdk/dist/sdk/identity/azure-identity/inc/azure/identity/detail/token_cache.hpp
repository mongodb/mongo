// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Token cache.
 *
 */

#pragma once

#include <azure/core/credentials/credentials.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <tuple>

namespace Azure { namespace Identity { namespace _detail {
  /**
   * @brief Access token cache.
   *
   */
  class TokenCache
#if !defined(_azure_TESTING_BUILD)
      final
#endif
  {
#if !defined(_azure_TESTING_BUILD)
  private:
#else
  protected:
#endif
    // A test hook that gets invoked before cache write lock gets acquired.
    virtual void OnBeforeCacheWriteLock() const {};

    // A test hook that gets invoked before item write lock gets acquired.
    virtual void OnBeforeItemWriteLock() const {};

    struct CacheKey
    {
      std::string Scope;
      std::string TenantId;
    };

    struct CacheKeyComparator
    {
      bool operator()(CacheKey const& lhs, CacheKey const& rhs) const
      {
        return std::tie(lhs.Scope, lhs.TenantId) < std::tie(rhs.Scope, rhs.TenantId);
      }
    };

    struct CacheValue
    {
      Core::Credentials::AccessToken AccessToken;
      std::shared_timed_mutex ElementMutex;
    };

    mutable std::map<CacheKey, std::shared_ptr<CacheValue>, CacheKeyComparator> m_cache;
    mutable std::shared_timed_mutex m_cacheMutex;

  private:
    TokenCache(TokenCache const&) = delete;
    TokenCache& operator=(TokenCache const&) = delete;

    // Checks cache element if cached value should be reused. Caller should be holding ElementMutex.
    static bool IsFresh(
        std::shared_ptr<CacheValue> const& item,
        DateTime::duration minimumExpiration,
        std::chrono::system_clock::time_point now);

    // Gets item from cache, or creates it, puts into cache, and returns.
    std::shared_ptr<CacheValue> GetOrCreateValue(
        CacheKey const& key,
        DateTime::duration minimumExpiration) const;

  public:
    TokenCache() = default;
    ~TokenCache() = default;

    /**
     * @brief Attempts to get token from cache, and if not found, gets the token using the function
     * provided, caches it, and returns its value.
     *
     * @param scopeString Authentication scopes (or resource) as string.
     * @param tenantId TenantId for authentication.
     * @param minimumExpiration Minimum token lifetime for the cached value to be returned.
     * @param getNewToken Function to get the new token for the given \p scopeString, in case when
     * cache does not have it, or if its remaining lifetime is less than \p minimumExpiration.
     *
     * @return Authentication token.
     *
     */
    Core::Credentials::AccessToken GetToken(
        std::string const& scopeString,
        std::string const& tenantId,
        DateTime::duration minimumExpiration,
        std::function<Core::Credentials::AccessToken()> const& getNewToken) const;
  };
}}} // namespace Azure::Identity::_detail
