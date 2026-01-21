// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/core/diagnostics/logger.hpp"
#include "azure/core/dll_import_export.hpp"

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <type_traits>

namespace Azure { namespace Core { namespace Diagnostics { namespace _internal {

  /** @brief Internal Log class used for generating diagnostic logs.
   *
   * When components within the Azure SDK wish to emit diagnostic log messages, they should use the
   * Azure::Core::Diagnostics::_internal::Log class to generate those messages.
   *
   * The Log class methods integrate with the public diagnostic logging infrastructure to generate
   * log messages which will be captured by either the default logger or a customer provided logger.
   *
   * Usage:
   *
   * There are two primary interfaces to the Log class. The first (and most common) is to use the
   * Log::Write method to write a string to the configured logger. For example:
   *
   * ```cpp
   * using namespace Azure::Core::Diagnostics::_internal;
   * using namespace Azure::Core::Diagnostics;
   *     :
   *     :
   * Log::Write(Logger::Level::Verbose, "This is a diagnostic message");
   * ```
   *
   * this will pass the string "This is a diagnostic message" to the configured logger at the
   * "Verbose" error level.
   *
   * The second interface is to use the Log::Stream() class to stream a string to the configured
   * logger. For example:
   *
   * ```cpp
   * using namespace Azure::Core::Diagnostics::_internal;
   * using namespace Azure::Core::Diagnostics;
   *     :
   *     :
   * int resultCode = 500;
   * Log::Stream(Logger::Level::Error) << "An error has occurred " << resultCode;
   * ```
   *
   * this will pass the string "An error has occurred 500" to the configured logger at the "Error"
   * error level.
   *
   */
  class Log final {
    static_assert(
        std::is_same<int, std::underlying_type<Logger::Level>::type>::value == true,
        "Logger::Level values must be representable as lock-free");

    static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic<int> must be lock-free");

    static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "atomic<bool> must be lock-free");

    static AZ_CORE_DLLEXPORT std::atomic<bool> g_isLoggingEnabled;
    static AZ_CORE_DLLEXPORT std::atomic<Logger::Level> g_logLevel;

    Log() = delete;
    ~Log() = delete;

  public:
    /** @brief Stream class used to enable using iomanip operators on an I/O stream.
     * Usage:
     *
     * ```cpp
     * using namespace Azure::Core::Diagnostics::_internal;
     * using namespace Azure::Core::Diagnostics;
     *     :
     *     :
     * int resultCode = 500;
     * Log::Stream(Logger::Level::Error) << "An error has occurred " << resultCode;
     * ```
     *
     * this will pass the string "An error has occurred 500" to the configured logger at the "Error"
     * error level.
     *
     * @remarks The Log::Stream() construct creates a temporary Stream object whose lifetime ends a
     * the end of the statement creating the Stream object. In the destructor for the stream, the
     * underlying stream object is flushed thus ensuring that the output is generated at the end of
     * the statement, even if the caller does not insert the std::endl object.
     */
    class Stream final {
    public:
      /** @brief Construct a new Stream object with the configured I/O level.
       *
       * @param level - Represents the desired diagnostic level for the operation.
       */
      Stream(Logger::Level level) : m_level{level} {}
      /** @brief Called when the Stream object goes out of scope. */
      ~Stream() { Log::Write(m_level, m_stream.str()); }
      Stream(Stream const&) = delete;
      Stream& operator=(Stream const&) = delete;

      /** @brief Insert an object of type T into the output stream.
       *
       * @tparam T Type of the object being inserted.
       * @param val value to be inserted into the underlying stream.
       */
      template <typename T> std::ostream& operator<<(T val) { return m_stream << val; }

    private:
      std::stringstream m_stream;
      Logger::Level m_level;
    };

    /** @brief Returns true if the logger would write a string at the specified level.
     *
     * This function primarily exists to enable callers to avoid expensive computations if the
     * currently configured log level doesn't support logging at the specified level.
     *
     * @param level - log level to check.
     * @returns true if the logger will write at that log level.
     */
    static bool ShouldWrite(Logger::Level level)
    {
      return g_isLoggingEnabled && level >= g_logLevel;
    }

    /** @brief Write a string to the configured logger at the specified log level.
     *
     * Expected usage:
     *
     * ```cpp
     * using namespace Azure::Core::Diagnostics::_internal;
     * using namespace Azure::Core::Diagnostics;
     *     :
     *     :
     * Log::Write(Logger::Level::Verbose, "This is a diagnostic message");
     * ```
     *
     * this will pass the string "This is a diagnostic message" to the configured logger at the
     * "Verbose" error level.
     *
     * @param level - log level to use for the message.
     * @param message - message to write to the logger.
     *
     */
    static void Write(Logger::Level level, std::string const& message);

    /** @brief Enable logging.
     *
     * @param isEnabled - true if logging should be enabled, false if it should be disabled.
     */
    static void EnableLogging(bool isEnabled);

    /** @brief Set the current log level globally.
     *
     * Note that this overrides any customer configuration of the log level and should generally be
     * avoided.
     *
     * @param logLevel - New global log level.
     */
    static void SetLogLevel(Logger::Level logLevel);

  private:
  };
}}}} // namespace Azure::Core::Diagnostics::_internal
