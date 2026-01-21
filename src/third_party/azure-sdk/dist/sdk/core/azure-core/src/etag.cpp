// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/etag.hpp"

using Azure::ETag;

const ETag& ETag::Any()
{
  static ETag any = ETag("*");
  return any;
}
