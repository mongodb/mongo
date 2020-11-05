/*
 * Copyright 2018-present MongoDB, Inc.
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

#include "kms_message_private.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *
hexlify (const uint8_t *buf, size_t len)
{
   char *hex_chars = malloc (len * 2 + 1);
   KMS_ASSERT (hex_chars);

   char *p = hex_chars;
   size_t i;

   for (i = 0; i < len; i++) {
      p += sprintf (p, "%02x", buf[i]);
   }

   *p = '\0';

   return hex_chars;
}

/* Returns -1 on error. */
int
unhexlify (const char *in, size_t len)
{
   int i;
   int byte;
   int total = 0;
   int multiplier = 1;

   for (i = (int) len - 1; i >= 0; i--) {
      char c = *(in + i);

      if (c >= '0' && c <= '9') {
         byte = c - 48;
      } else if (c >= 'a' && c <= 'f') {
         byte = c - 97 + 10;
      } else if (c >= 'A' && c <= 'F') {
         byte = c - 65 + 10;
      } else {
         return -1;
      }

      total += byte * multiplier;
      multiplier *= 16;
   }
   return total;
}
