/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/Cache.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>

namespace Aws
{
    namespace Utils
    {
        template <typename TKey, typename TValue>
        class ConcurrentCache
        {
        public:
            explicit ConcurrentCache(size_t size = 1000) : m_cache(size) { }

            bool Get(const TKey& key, TValue& value) const
            {
                Aws::Utils::Threading::ReaderLockGuard g(m_rwlock);
                return m_cache.Get(key, value);
            }

            template<typename UValue>
            void Put(const TKey& key, UValue&& val, std::chrono::milliseconds duration)
            {
                Aws::Utils::Threading::WriterLockGuard g(m_rwlock);
                m_cache.Put(key, std::forward<UValue>(val), duration);
            }

            template<typename UValue>
            void Put(TKey&& key, UValue&& val, std::chrono::milliseconds duration)
            {
                Aws::Utils::Threading::WriterLockGuard g(m_rwlock);
                m_cache.Put(std::move(key), std::forward<UValue>(val), duration);
            }

            using TransformFunction = std::function<typename Aws::Utils::Cache<TKey, TValue>::Value(const TKey &,
                const typename Aws::Utils::Cache<TKey, TValue>::Value &)>;

            void Transform(TransformFunction function) {
                Aws::Utils::Threading::WriterLockGuard g(m_rwlock);
                m_cache.Transform(function);
            }

            using FilterFunction = std::function<bool(const TKey &,
                const typename Aws::Utils::Cache<TKey, TValue>::Value &)>;

            void Filter(FilterFunction function) {
                Aws::Utils::Threading::WriterLockGuard g(m_rwlock);
                m_cache.Filter(function);
            }

        private:
            Aws::Utils::Cache<TKey, TValue> m_cache;
            mutable Aws::Utils::Threading::ReaderWriterLock m_rwlock;
        };
    }
}
