/*
 * Copyright 2009-present MongoDB, Inc.
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

#include "common-thread-private.h"

#include <errno.h>

#if defined(BSON_OS_UNIX)
int
mcommon_thread_create (bson_thread_t *thread, BSON_THREAD_FUN_TYPE (func), void *arg)
{
   BSON_ASSERT_PARAM (thread);
   BSON_ASSERT_PARAM (func);
   BSON_OPTIONAL_PARAM (arg); // optional.
   return pthread_create (thread, NULL, func, arg);
}
int
mcommon_thread_join (bson_thread_t thread)
{
   return pthread_join (thread, NULL);
}

#if defined(MONGOC_ENABLE_DEBUG_ASSERTIONS) && defined(BSON_OS_UNIX)
bool
mcommon_mutex_is_locked (bson_mutex_t *mutex)
{
   return mutex->valid_tid && pthread_equal (pthread_self (), mutex->lock_owner);
}
#endif

#else
int
mcommon_thread_create (bson_thread_t *thread, BSON_THREAD_FUN_TYPE (func), void *arg)
{
   BSON_ASSERT_PARAM (thread);
   BSON_ASSERT_PARAM (func);
   BSON_OPTIONAL_PARAM (arg); // optional.

   *thread = (HANDLE) _beginthreadex (NULL, 0, func, arg, 0, NULL);
   if (0 == *thread) {
      return errno;
   }
   return 0;
}
int
mcommon_thread_join (bson_thread_t thread)
{
   int ret;

   /* zero indicates success for WaitForSingleObject. */
   ret = WaitForSingleObject (thread, INFINITE);
   if (WAIT_OBJECT_0 != ret) {
      return ret;
   }
   /* zero indicates failure for CloseHandle. */
   ret = CloseHandle (thread);
   if (0 == ret) {
      return 1;
   }
   return 0;
}
#endif
