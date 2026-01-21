// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Define a convenience layer on top of Json for setting optional fields.
 *
 */

#pragma once

#include "azure/core/internal/json/json.hpp"
#include "azure/core/nullable.hpp"

#include <functional>
#include <string>

namespace Azure { namespace Core { namespace Json { namespace _internal {

  /**
   * @brief Define a wrapper for working with Json containing optional fields.
   *
   */
  struct JsonOptional final
  {
    /**
     * @brief If the optional key \p key is present in the json node \p jsonKey set the value of \p
     * the Nullable destination.
     *
     * @remark If the key is not in the json node, the \p destination is not modified.
     *
     * @param jsonKey The json node to review.
     * @param key The key name for the optional property.
     * @param destination The value to update if the key name property is in the json node.
     */
    template <class T>
    static inline void SetIfExists(
        Azure::Nullable<T>& destination,
        Azure::Core::Json::_internal::json const& jsonKey,
        std::string const& key) noexcept
    {
      if (jsonKey.contains(key) && !jsonKey[key].is_null()) // In Json and not-Null
      {
        destination = jsonKey[key].get<T>();
      }
    }

    /**
     * @brief If the optional key \p key is present in the json node \p jsonKey set the value of \p
     * destination.
     *
     * @remark If the key is not in the json node, the \p destination is not modified.
     *
     * @param jsonKey The json node to review.
     * @param key The key name for the optional property.
     * @param destination The value to update if the key name property is in the json node.
     * @param decorator A callback used to convert the Json value from `V` type to the `T` type. For
     * example, getting std::string from Json (the V type) and setting a Nullable<Datatime> (where T
     * type is Datetime), the decorator would define how to create the Datetime from the
     * std::string.
     */
    template <class V, class T>
    static inline void SetIfExists(
        T& destination,
        Azure::Core::Json::_internal::json const& jsonKey,
        std::string const& key,
        std::function<T(V value)> decorator) noexcept
    {
      if (jsonKey.contains(key))
      {
        if (!jsonKey[key].is_null())
        {
          destination = decorator(jsonKey[key].get<V>());
        }
      }
    }

    /**
     * @brief If the optional key \p key is present in the json node \p jsonKey set the value of \p
     * the Nullable destination.
     *
     * @remark If the key is not in the json node, the \p destination is not modified.
     *
     * @param jsonKey The json node to review.
     * @param key The key name for the optional property.
     * @param destination The value to update if the key name property is in the json node.
     * @param decorator A optional function to update the json value before updating the \p
     * destination.
     */
    template <class T, class V>
    static inline void SetIfExists(
        Azure::Nullable<V>& destination,
        Azure::Core::Json::_internal::json const& jsonKey,
        std::string const& key,
        std::function<V(T value)> decorator) noexcept
    {
      if (jsonKey.contains(key))
      {
        destination = decorator(jsonKey[key].get<T>());
      }
    }

    template <class T>
    static inline void SetFromIfPredicate(
        T const& source,
        std::function<bool(T const&)> predicate,
        Azure::Core::Json::_internal::json& jsonKey,
        std::string const& keyName,
        std::function<std::string(T const&)> decorator)
    {
      if (predicate(source))
      {
        jsonKey[keyName] = decorator(source);
      }
    }

    template <class T, class R>
    static inline void SetFromNullable(
        Azure::Nullable<T> const& source,
        Azure::Core::Json::_internal::json& jsonKey,
        std::string const& keyName,
        std::function<R(T const&)> factory)
    {
      if (source)
      {
        jsonKey[keyName] = factory(source.Value());
      }
    }

    template <class T>
    static inline void SetFromNullable(
        Azure::Nullable<T> const& source,
        Azure::Core::Json::_internal::json& jsonKey,
        std::string const& keyName)
    {
      if (source)
      {
        jsonKey[keyName] = source.Value();
      }
    }
  };
}}}} // namespace Azure::Core::Json::_internal
