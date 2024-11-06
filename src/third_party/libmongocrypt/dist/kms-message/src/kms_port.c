/*
 * Copyright 2020-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kms_port.h"
#if defined(_WIN32)
#include <stdlib.h>
#include <string.h>
char *
kms_strndup (const char *src, size_t len)
{
   char *dst = (char *) malloc (len + 1);
   if (!dst) {
      return 0;
   }

   memcpy (dst, src, len);
   dst[len] = '\0';

   return dst;
}
#endif
