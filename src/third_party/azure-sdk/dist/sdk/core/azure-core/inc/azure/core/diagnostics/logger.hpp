// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Handling log messages from Azure SDK.
 */

#pragma once

#include <functional>
#include <string>

namespace Azure { namespace Core { namespace Diagnostics {
  /**
   * @brief Log message handler.
   */
  class Logger final {
  public:
    /**
     * @brief Log message level.
     *
     */
    // https://github.com/Azure/azure-sdk-for-java/blob/main/sdk/core/azure-core/src/main/java/com/azure/core/util/logging/LogLevel.java
    enum class Level : int
    {
      /// Logging level for detailed troubleshooting scenarios.
      Verbose = 1,

      /// Logging level when a function operates normally.
      Informational = 2,

      /// Logging level when a function fails to perform its intended task.
      Warning = 3,

      /// Logging level for failures that the application is unlikely to recover from.
      Error = 4,
    };

    /**
     * @brief Sets the function that will be invoked to report an Azure SDK log message.
     *
     * @param listener A callback function that will be invoked when the SDK reports a log message.
     * If `nullptr`, no function will be invoked.
     */
    static void SetListener(std::function<void(Level level, std::string const& message)> listener);

    /**
     * @brief Sets the log message level an application is interested in receiving.
     *
     * @param level The most verbose level to receive log messages for. Least verbose levels will be
     * included, more verbose levels will be not.
     */
    static void SetLevel(Level level);

  private:
    /**
     * @brief An instance of `%Logger` class cannot be created.
     *
     */
    Logger() = delete;

    /**
     * @brief An instance of `%Logger` class cannot be destructed, because no instance can be
     * created.
     *
     */
    ~Logger() = delete;
  };
}}} // namespace Azure::Core::Diagnostics
