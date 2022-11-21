/*
 * Copyright 2019-present MongoDB, Inc.
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

#include "../mongocrypt-mutex-private.h"

#ifndef _WIN32

void
_mongocrypt_mutex_init (mongocrypt_mutex_t *mutex)
{
   int ret = pthread_mutex_init (mutex, NULL);
   if (ret) {
      abort ();
   }
}

void
_mongocrypt_mutex_cleanup (mongocrypt_mutex_t *mutex)
{
   int ret = pthread_mutex_destroy (mutex);
   if (ret) {
      abort ();
   }
}

void
_mongocrypt_mutex_lock (mongocrypt_mutex_t *mutex)
{
   int ret = pthread_mutex_lock (mutex);
   if (ret) {
      abort ();
   }
}

void
_mongocrypt_mutex_unlock (mongocrypt_mutex_t *mutex)
{
   int ret = pthread_mutex_unlock (mutex);
   if (ret) {
      abort ();
   }
}

#endif /* _WIN32 */