// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include <exception>
#include <iostream>

namespace Azure { namespace Core { namespace Diagnostics { namespace _internal {
  /**
   * @brief Global Exception handler used for test collateral. This is not intended to be used by
   * any non-test code.
   *
   * The GlobalExceptionHandler class is used to catch any unhandled exceptions and report them as a
   * part of test collateral, it is intended to be called as a SIGABRT handler set by:
   *
   * ```
   *  signal(SIGABRT, Azure::Core::Diagnostics::_internal::GlobalExceptionHandler);
   * ```
   *
   */
  struct GlobalExceptionHandler
  {
    static void HandleSigAbort(int)
    {
      // Rethrow any exceptions on the current stack - this will cause any pending exceptions to
      // be thrown so we can catch them and report them to the caller. This is needed because the
      // terminate() function on Windows calls abort() which normally pops up a modal dialog box
      // after which it terminates the application without reporting the exception.
      try
      {
        throw;
      }
      catch (std::exception const& ex)
      {
        std::cerr << "SIGABRT raised, exception: " << ex.what() << std::endl;
      }
    }
  };
}}}} // namespace Azure::Core::Diagnostics::_internal
