/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/platform/Security.h>

#include <string.h>

namespace Aws
{
namespace Security
{

void SecureMemClear(unsigned char *data, size_t length)
{
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__DragonFly__)
    memset_s(data, length, 0, length);
#else
    memset(data, 0, length);
    asm volatile("" : "+m" (data));
#endif
}

} // namespace Security
} // namespace Aws
