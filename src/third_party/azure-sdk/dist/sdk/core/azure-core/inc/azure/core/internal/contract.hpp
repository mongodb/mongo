// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#if defined(NO_CONTRACTS_CHECKING)
#define AZ_CONTRACT(condition, error)
#define AZ_CONTRACT_ARG_NOT_NULL(arg)
#else
#define AZ_CONTRACT(condition, error) \
  do \
  { \
    if (!(condition)) \
    { \
      return error; \
    } \
  } while (0)

#define AZ_CONTRACT_ARG_NOT_NULL(arg) AZ_CONTRACT((arg) != NULL, 1)

#endif
