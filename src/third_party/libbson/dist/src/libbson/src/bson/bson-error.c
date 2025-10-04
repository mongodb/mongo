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


#include <stdio.h>
#include <stdarg.h>

#include <bson/bson-compat.h>
#include <bson/bson-config.h>
#include <bson/bson-error.h>
#include <bson/bson-memory.h>
#include <bson/bson-string.h>
#include <bson/bson-types.h>

// See `bson_strerror_r()` definition below.
#if !defined(_WIN32) && !defined(__APPLE__)
#include <locale.h> // uselocale()
#endif


/*
 *--------------------------------------------------------------------------
 *
 * bson_set_error --
 *
 *       Initializes @error using the parameters specified.
 *
 *       @domain is an application specific error domain which should
 *       describe which module initiated the error. Think of this as the
 *       exception type.
 *
 *       @code is the @domain specific error code.
 *
 *       @format is used to generate the format string. It uses vsnprintf()
 *       internally so the format should match what you would use there.
 *
 * Parameters:
 *       @error: A #bson_error_t.
 *       @domain: The error domain.
 *       @code: The error code.
 *       @format: A printf style format string.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       @error is initialized.
 *
 *--------------------------------------------------------------------------
 */

void
bson_set_error (bson_error_t *error, /* OUT */
                uint32_t domain,     /* IN */
                uint32_t code,       /* IN */
                const char *format,  /* IN */
                ...)                 /* IN */
{
   va_list args;

   if (error) {
      error->domain = domain;
      error->code = code;

      va_start (args, format);
      bson_vsnprintf (error->message, sizeof error->message, format, args);
      va_end (args);

      error->message[sizeof error->message - 1] = '\0';
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_strerror_r --
 *
 *       This is a reentrant safe macro for strerror.
 *
 *       The resulting string may be stored in @buf.
 *
 * Returns:
 *       A pointer to a static string or @buf.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

char *
bson_strerror_r (int err_code,                    /* IN */
                 char *buf BSON_MAYBE_UNUSED,     /* IN */
                 size_t buflen BSON_MAYBE_UNUSED) /* IN */
{
   static const char *unknown_msg = "Unknown error";
   char *ret = NULL;

#if defined(_WIN32)
   // Windows does not provide `strerror_l` or `strerror_r`, but it does
   // unconditionally provide `strerror_s`.
   if (strerror_s (buf, buflen, err_code) != 0) {
      ret = buf;
   }
#elif defined(_AIX)
   // AIX does not provide strerror_l, and its strerror_r isn't glibc's.
   // But it does provide a glibc compatible one called __linux_strerror_r
   ret = __linux_strerror_r (err_code, buf, buflen);
#elif defined(__APPLE__)
   // Apple does not provide `strerror_l`, but it does unconditionally provide
   // the XSI-compliant `strerror_r`, but only when compiling with Apple Clang.
   // GNU extensions may still be a problem if we are being compiled with GCC on
   // Apple. Avoid the compatibility headaches with GNU extensions and the musl
   // library by assuming the implementation will not cause UB when reading the
   // error message string even when `strerror_r` fails, as encouraged (but not
   // required) by the POSIX spec (see:
   // https://pubs.opengroup.org/onlinepubs/9699919799/functions/strerror.html#tag_16_574_08).
   (void) strerror_r (err_code, buf, buflen);
#elif defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 700
   // The behavior (of `strerror_l`) is undefined if the locale argument to
   // `strerror_l()` is the special locale object LC_GLOBAL_LOCALE or is not a
   // valid locale object handle.
   locale_t locale = uselocale ((locale_t) 0);
   // No need to test for error (it can only be [EINVAL]).
   if (locale == LC_GLOBAL_LOCALE) {
      // Only use our own locale if a thread-local locale was not already set.
      // This is just to satisfy `strerror_l`. We do NOT want to unconditionally
      // set a thread-local locale.
      locale = newlocale (LC_MESSAGES_MASK, "C", (locale_t) 0);
   }
   BSON_ASSERT (locale != LC_GLOBAL_LOCALE);

   // Avoid `strerror_r` compatibility headaches with GNU extensions and the
   // musl library by using `strerror_l` instead. Furthermore, `strerror_r` is
   // scheduled to be marked as obsolete in favor of `strerror_l` in the
   // upcoming POSIX Issue 8 (see:
   // https://www.austingroupbugs.net/view.php?id=655).
   //
   // POSIX Spec: since strerror_l() is required to return a string for some
   // errors, an application wishing to check for all error situations should
   // set errno to 0, then call strerror_l(), then check errno.
   if (locale != (locale_t) 0) {
      errno = 0;
      ret = strerror_l (err_code, locale);

      if (errno != 0) {
         ret = NULL;
      }

      freelocale (locale);
   } else {
      // Could not obtain a valid `locale_t` object to satisfy `strerror_l`.
      // Fallback to `bson_strncpy` below.
   }
#elif defined(_GNU_SOURCE)
   // Unlikely, but continue supporting use of GNU extension in cases where the
   // C Driver is being built without _XOPEN_SOURCE=700.
   ret = strerror_r (err_code, buf, buflen);
#else
#error "Unable to find a supported strerror_r candidate"
#endif

   if (!ret) {
      bson_strncpy (buf, unknown_msg, buflen);
      ret = buf;
   }

   return ret;
}
