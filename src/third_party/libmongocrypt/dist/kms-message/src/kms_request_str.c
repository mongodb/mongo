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

#include "hexlify.h"
#include "kms_crypto.h"
#include "kms_message/kms_message.h"
#include "kms_message_private.h"
#include "kms_request_str.h"
#include "kms_port.h"

#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h> /* CHAR_BIT */

bool rfc_3986_tab[256] = {0};
bool kms_initialized = false;

static void
tables_init (void)
{
   int i;

   if (kms_initialized) {
      return;
   }

   for (i = 0; i < 256; ++i) {
      rfc_3986_tab[i] =
         isalnum (i) || i == '~' || i == '-' || i == '.' || i == '_';
   }

   kms_initialized = true;
}


kms_request_str_t *
kms_request_str_new (void)
{
   kms_request_str_t *s = malloc (sizeof (kms_request_str_t));
   KMS_ASSERT (s);

   s->len = 0;
   s->size = 16;
   s->str = malloc (s->size);
   KMS_ASSERT (s->str);

   s->str[0] = '\0';

   return s;
}

kms_request_str_t *
kms_request_str_new_from_chars (const char *chars, ssize_t len)
{
   kms_request_str_t *s = malloc (sizeof (kms_request_str_t));
   KMS_ASSERT (s);

   size_t actual_len;

   actual_len = len < 0 ? strlen (chars) : (size_t) len;
   s->size = actual_len + 1;
   s->str = malloc (s->size);
   KMS_ASSERT (s->str);

   memcpy (s->str, chars, actual_len);
   s->str[actual_len] = '\0';
   s->len = actual_len;

   return s;
}

kms_request_str_t *
kms_request_str_wrap (char *chars, ssize_t len)
{
   kms_request_str_t *s;

   if (!chars) {
      return NULL;
   }

   s = malloc (sizeof (kms_request_str_t));
   KMS_ASSERT (s);


   s->str = chars;
   s->len = len < 0 ? strlen (chars) : (size_t) len;
   s->size = s->len;

   return s;
}

void
kms_request_str_destroy (kms_request_str_t *str)
{
   if (!str) {
      return;
   }

   free (str->str);
   free (str);
}

char *
kms_request_str_detach (kms_request_str_t *str)
{
   if (!str) {
      return NULL;
   }
   char *r = str->str;
   free (str);
   return r;
}

const char *
kms_request_str_get (kms_request_str_t *str)
{
   return str->str;
}

bool
kms_request_str_reserve (kms_request_str_t *str, size_t size)
{
   size_t next_size = str->len + size + 1;

   if (str->size < next_size) {
      /* next power of 2 */
      --next_size;
      next_size |= next_size >> 1U;
      next_size |= next_size >> 2U;
      next_size |= next_size >> 4U;
      next_size |= next_size >> 8U;
      next_size |= next_size >> 16U;
      ++next_size;

      str->size = next_size;
      str->str = realloc (str->str, next_size);
   }

   return str->str != NULL;
}

kms_request_str_t *
kms_request_str_dup (kms_request_str_t *str)
{
   kms_request_str_t *dup = malloc (sizeof (kms_request_str_t));
   KMS_ASSERT (dup);


   dup->str = kms_strndup (str->str, str->len);
   dup->len = str->len;
   dup->size = str->len + 1;

   return dup;
}

void
kms_request_str_set_chars (kms_request_str_t *str,
                           const char *chars,
                           ssize_t len)
{
   size_t actual_len = len < 0 ? strlen (chars) : (size_t) len;
   kms_request_str_reserve (str, actual_len); /* adds 1 for nil */
   memcpy (str->str, chars, actual_len + 1);
   str->len = actual_len;
}

bool
kms_request_str_ends_with (kms_request_str_t *str, kms_request_str_t *suffix)
{
   if (str->len >= suffix->len &&
       0 == strncmp (
               &str->str[str->len - suffix->len], suffix->str, suffix->len)) {
      return true;
   }

   return false;
}

void
kms_request_str_append (kms_request_str_t *str, kms_request_str_t *appended)
{
   size_t next_len = str->len + appended->len;

   kms_request_str_reserve (str, next_len);
   memcpy (str->str + str->len, appended->str, appended->len);
   str->len += appended->len;
   str->str[str->len] = '\0';
}

void
kms_request_str_append_char (kms_request_str_t *str, char c)
{
   kms_request_str_reserve (str, 1);
   *(str->str + str->len) = c;
   ++str->len;
   str->str[str->len] = '\0';
}


void
kms_request_str_append_chars (kms_request_str_t *str,
                              const char *appended,
                              ssize_t len)
{
   size_t str_len;
   if (len < 0) {
      str_len = strlen (appended);
   } else {
      str_len = (size_t) len;
   }
   kms_request_str_reserve (str, str_len);
   memcpy (str->str + str->len, appended, str_len);
   str->len += str_len;
   str->str[str->len] = '\0';
}

void
kms_request_str_append_newline (kms_request_str_t *str)
{
   kms_request_str_append_char (str, '\n');
}

void
kms_request_str_append_lowercase (kms_request_str_t *str,
                                  kms_request_str_t *appended)
{
   size_t i;
   char *p;

   i = str->len;
   kms_request_str_append (str, appended);

   /* downcase the chars from the old end to the new end of str */
   for (; i < str->len; ++i) {
      p = &str->str[i];
      /* ignore UTF-8 non-ASCII chars, which have 1 in the top bit */
      if (((unsigned int) (*p) & (0x1U << 7U)) == 0) {
         *p = (char) tolower (*p);
      }
   }
}

void
kms_request_str_appendf (kms_request_str_t *str, const char *format, ...)
{
   va_list args;
   size_t remaining;
   int n;

   KMS_ASSERT (format);

   while (true) {
      remaining = str->size - str->len;

      va_start (args, format);
      n = vsnprintf (&str->str[str->len], remaining, format, args);
      va_end (args);

      if (n > -1 && (size_t) n < remaining) {
         /* success */
         str->len += (size_t) n;
         return;
      }

      if (n > -1) {
         kms_request_str_reserve (str, (size_t) n);
      } else {
         /* TODO: error! */
         abort ();
      }
   }
}

void
kms_request_str_append_escaped (kms_request_str_t *str,
                                kms_request_str_t *appended,
                                bool escape_slash)
{
   uint8_t *in;
   uint8_t *out;
   size_t i;

   tables_init ();

   /* might replace each input char with 3 output chars: "%AB" */
   kms_request_str_reserve (str, 3 * appended->len);
   in = (uint8_t *) appended->str;
   out = (uint8_t *) str->str + str->len;

   for (i = 0; i < appended->len; ++i) {
      if (rfc_3986_tab[*in] || (*in == '/' && !escape_slash)) {
         *out = *in;
         ++out;
         ++str->len;
      } else {
         sprintf ((char *) out, "%%%02X", *in);
         out += 3;
         str->len += 3;
      }

      ++in;
   }
}

void
kms_request_str_append_stripped (kms_request_str_t *str,
                                 kms_request_str_t *appended)
{
   const char *src = appended->str;
   const char *end = appended->str + appended->len;
   bool space = false;
   bool comma = false;

   kms_request_str_reserve (str, appended->len);

   /* msvcrt is unhappy when it gets non-ANSI characters in isspace */
   while (*src >= 0 && isspace (*src)) {
      ++src;
   }

   while (src < end) {
      /* replace newlines with commas. not documented but see
       * get-header-value-multiline.creq */
      if (*src == '\n') {
         comma = true;
         space = false;
      } else if (*src >= 0 && isspace (*src)) {
         space = true;
      } else {
         if (comma) {
            kms_request_str_append_char (str, ',');
            comma = false;
            space = false;
         }

         /* is there a run of spaces waiting to be written as one space? */
         if (space) {
            kms_request_str_append_char (str, ' ');
            space = false;
         }

         kms_request_str_append_char (str, *src);
      }

      ++src;
   }
}

bool
kms_request_str_append_hashed (_kms_crypto_t *crypto,
                               kms_request_str_t *str,
                               kms_request_str_t *appended)
{
   uint8_t hash[32] = {0};
   char *hex_chars;

   if (!crypto->sha256 (crypto->ctx, appended->str, appended->len, hash)) {
      return false;
   }

   hex_chars = hexlify (hash, sizeof (hash));
   kms_request_str_append_chars (str, hex_chars, 2 * sizeof (hash));
   free (hex_chars);

   return true;
}

bool
kms_request_str_append_hex (kms_request_str_t *str,
                            unsigned char *data,
                            size_t len)
{
   char *hex_chars;

   hex_chars = hexlify (data, len);
   KMS_ASSERT (len <= SSIZE_MAX / 2);
   kms_request_str_append_chars (str, hex_chars, (ssize_t) (len * 2));
   free (hex_chars);

   return true;
}

static bool
starts_with (char *s, const char *prefix)
{
   if (strstr (s, prefix) == s) {
      return true;
   }

   return false;
}

/* remove from last slash to the end, but don't remove slash from start */
static void
delete_last_segment (kms_request_str_t *str, bool is_absolute)
{
   ssize_t i;

   if (!str->len) {
      return;
   }

   KMS_ASSERT (str->len < SSIZE_MAX);
   for (i = (ssize_t) str->len - 1; i >= 0; --i) {
      if (str->str[i] == '/') {
         if (i == 0 && is_absolute) {
            str->len = 1;
         } else {
            str->len = (size_t) i;
         }

         goto done;
      }
   }

   /* no slashes */
   str->len = 0;

done:
   str->str[str->len] = '\0';
}

/* follow algorithm in https://tools.ietf.org/html/rfc3986#section-5.2.4,
 * the block comments are copied from there */
kms_request_str_t *
kms_request_str_path_normalized (kms_request_str_t *str)
{
   kms_request_str_t *slash = kms_request_str_new_from_chars ("/", 1);
   kms_request_str_t *out = kms_request_str_new ();
   char *in = strdup (str->str);
   char *p = in;
   char *end = in + str->len;
   bool is_absolute = (*p == '/');

   if (0 == strcmp (p, "/")) {
      goto done;
   }

   while (p < end) {
      /* If the input buffer begins with a prefix of "../" or "./",
       * then remove that prefix from the input buffer */
      if (starts_with (p, "../")) {
         p += 3;
      } else if (starts_with (p, "./")) {
         p += 2;
      }
      /* otherwise, if the input buffer begins with a prefix of "/./" or "/.",
       * where "." is a complete path segment, then replace that prefix with "/"
       * in the input buffer */
      else if (starts_with (p, "/./")) {
         p += 2;
      } else if (0 == strcmp (p, "/.")) {
         break;
      }
      /* otherwise, if the input buffer begins with a prefix of "/../" or "/..",
       * where ".." is a complete path segment, then replace that prefix with
       * "/" in the input buffer and remove the last segment and its preceding
       * "/" (if any) from the output buffer */
      else if (starts_with (p, "/../")) {
         p += 3;
         delete_last_segment (out, is_absolute);
      } else if (0 == strcmp (p, "/..")) {
         delete_last_segment (out, is_absolute);
         break;
      }
      /* otherwise, if the input buffer consists only of "." or "..", then
         remove that from the input buffer */
      else if (0 == strcmp (p, ".") || 0 == strcmp (p, "..")) {
         break;
      }
      /* otherwise, move the first path segment in the input buffer to the end
       * of the output buffer, including the initial "/" character (if any) and
       * any subsequent characters up to, but not including, the next "/"
       * character or the end of the input buffer. */
      else {
         char *next_slash = strchr (p + 1, '/');
         if (!next_slash) {
            next_slash = end;
         }

         /* fold repeated slashes */
         if (kms_request_str_ends_with (out, slash) && *p == '/') {
            ++p;
         }

         /* normalize "a/../b" as "b", not as "/b" */
         if (out->len == 0 && !is_absolute && *p == '/') {
            ++p;
         }

         kms_request_str_append_chars (out, p, next_slash - p);
         p = next_slash;
      }
   }

done:
   free (in);
   kms_request_str_destroy (slash);

   if (!out->len) {
      kms_request_str_append_char (out, '/');
   }

   return out;
}
