/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"){}
 *
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

#include "kms_message/kms_b64.h"
#include "kms_message/kms_message.h"
#include "kms_message_private.h"
#include "kms_crypto.h"

#include <stdarg.h>
#include <stdio.h>

void
set_error (char *error, size_t size, const char *fmt, ...)
{
   va_list va;

   va_start (va, fmt);
   (void) vsnprintf (error, size, fmt, va);
   va_end (va);
}

int
kms_message_init (void)
{
   kms_message_b64_initialize_rmap ();
   return kms_crypto_init ();
}

void
kms_message_cleanup (void)
{
   kms_crypto_cleanup ();
}
