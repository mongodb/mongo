// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/azure_assert.hpp"

// Calling this function would terminate program, therefore this function can't be covered in tests.
[[noreturn]] void Azure::Core::_internal::AzureNoReturnPath(std::string const& msg)
{
  // void msg for Release build where Assert is ignored
  (void)msg;
  AZURE_ASSERT_MSG(false, msg);
  std::abort();
}
