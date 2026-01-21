// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/internal/environment.hpp"

#include "azure/core/platform.hpp"

#if defined(AZ_PLATFORM_WINDOWS)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <windows.h>

#include <vector>
#else
#include <stdlib.h>
#endif

using Azure::Core::_internal::Environment;

std::string Environment::GetVariable(const char* name)
{
  if (name != nullptr && name[0] != 0)
  {
#if defined(AZ_PLATFORM_WINDOWS)
    std::vector<std::string::value_type> bufferVector;
    std::string::value_type* buffer = nullptr;
    DWORD bufferSize = 0;
    while (const auto requiredSize = GetEnvironmentVariableA(name, buffer, bufferSize))
    {
      if (requiredSize < bufferSize)
      {
        return std::string(buffer, buffer + (bufferSize - 1));
      }

      bufferVector.resize(static_cast<decltype(bufferVector)::size_type>(requiredSize));
      bufferSize = requiredSize;
      buffer = bufferVector.data();
    }
#else
    if (const auto value = getenv(name))
    {
      return std::string(value);
    }
#endif
  }
  return std::string();
}

void Environment::SetVariable(const char* name, const char* value)
{
  if (name != nullptr && name[0] != 0)
  {
    const auto isEmptyValue = (value == nullptr || value[0] == 0);

#if defined(AZ_PLATFORM_WINDOWS)
    static_cast<void>(SetEnvironmentVariableA(name, isEmptyValue ? nullptr : value));
#else
    if (isEmptyValue)
    {
      static_cast<void>(unsetenv(name));
    }
    else
    {
      static_cast<void>(setenv(name, value, 1));
    }
#endif
  }
}
