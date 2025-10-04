// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <string>

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{

/**
  Read a boolean environment variable.
  @param env_var_name Environment variable name
  @param [out] value Variable value, if it exists
  @return true if the variable exists
*/
bool GetBoolEnvironmentVariable(const char *env_var_name, bool &value);

/**
  Read a duration environment variable.
  @param env_var_name Environment variable name
  @param [out] value Variable value, if it exists
  @return true if the variable exists
*/
bool GetDurationEnvironmentVariable(const char *env_var_name,
                                    std::chrono::system_clock::duration &value);

/**
  Read a string environment variable.
  @param env_var_name Environment variable name
  @param [out] value Variable value, if it exists
  @return true if the variable exists
*/
bool GetStringEnvironmentVariable(const char *env_var_name, std::string &value);

#if defined(_MSC_VER)
inline int setenv(const char *name, const char *value, int)
{
  return _putenv_s(name, value);
}

inline int unsetenv(const char *name)
{
  return setenv(name, "", 1);
}
#endif

}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
