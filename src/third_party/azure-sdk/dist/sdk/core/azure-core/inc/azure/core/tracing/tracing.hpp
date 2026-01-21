// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Public TracerProvider type used to represent a tracer provider.
 */

#pragma once

#include <azure/core/context.hpp>

#include <memory>
#include <string>

namespace Azure { namespace Core { namespace Tracing {
  class TracerProvider;
  namespace _internal {
    class Tracer;
    /**
     * @brief Trace Provider - factory for creating Tracer objects.
     */
    class TracerProviderImpl {
    public:
      /**
       * @brief Create a Tracer object
       *
       * @param name Name of the tracer object, typically the name of the Service client
       * (Azure.Storage.Blobs, for example)
       * @param version Optional version of the service client.
       * @return std::shared_ptr<Azure::Core::Tracing::Tracer>
       */
      virtual std::shared_ptr<Azure::Core::Tracing::_internal::Tracer> CreateTracer(
          std::string const& name,
          std::string const& version = {}) const = 0;

      virtual ~TracerProviderImpl() = default;
    };

    struct TracerProviderImplGetter
    {
      /**
       * @brief Returns a TracerProviderImpl from a TracerProvider object.
       *
       * @param provider The TracerProvider object.
       * @returns A TracerProviderImpl implementation.
       */
      static std::shared_ptr<TracerProviderImpl> TracerImplFromTracer(
          std::shared_ptr<TracerProvider> const& provider);
    };

  } // namespace _internal

  /**
   * @brief Trace Provider - factory for creating Tracer objects.
   */
  class TracerProvider : private _internal::TracerProviderImpl {
    // Marked TracerImplFromTracer as friend so it can access private members in the class.
    friend std::shared_ptr<TracerProviderImpl>
    _internal::TracerProviderImplGetter::TracerImplFromTracer(
        std::shared_ptr<TracerProvider> const&);
  };

}}} // namespace Azure::Core::Tracing
