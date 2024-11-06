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

#include "kms_crypto.h"
#include "kms_message/kms_message.h"
#include "kms_message_private.h"
#include "kms_request_opt_private.h"
#include "kms_port.h"
#include <limits.h> /* CHAR_BIT */

static kms_kv_list_t *
parse_query_params (kms_request_str_t *q)
{
   kms_kv_list_t *lst = kms_kv_list_new ();
   char *p = q->str;
   char *end = q->str + q->len;
   char *amp, *equals;
   kms_request_str_t *k, *v;

   do {
      equals = strchr ((const char *) p, '=');
      if (!equals) {
         kms_kv_list_destroy (lst);
         return NULL;
      }
      amp = strchr ((const char *) equals, '&');
      if (!amp) {
         amp = end;
      }

      k = kms_request_str_new_from_chars (p, equals - p);
      v = kms_request_str_new_from_chars (equals + 1, amp - equals - 1);
      kms_kv_list_add (lst, k, v);
      kms_request_str_destroy (k);
      kms_request_str_destroy (v);

      p = amp + 1;
   } while (p < end);

   return lst;
}

static bool
check_and_prohibit_kmip (kms_request_t *req)
{
   if (req->provider == KMS_REQUEST_PROVIDER_KMIP) {
      KMS_ERROR (req, "Function not applicable to KMIP");
      return false;
   }
   return true;
}

kms_request_t *
kms_request_new (const char *method,
                 const char *path_and_query,
                 const kms_request_opt_t *opt)
{
   kms_request_t *request = calloc (1, sizeof (kms_request_t));
   const char *question_mark;

   KMS_ASSERT (request);
   if (opt && opt->provider) {
      request->provider = opt->provider;
   } else {
      request->provider = KMS_REQUEST_PROVIDER_AWS;
   }

   if (!check_and_prohibit_kmip (request)) {
      return request;
   }
   /* parsing may set failed to true */
   request->failed = false;

   request->finalized = false;
   request->region = kms_request_str_new ();
   request->service = kms_request_str_new ();
   request->access_key_id = kms_request_str_new ();
   request->secret_key = kms_request_str_new ();

   question_mark = strchr (path_and_query, '?');
   if (question_mark) {
      request->path = kms_request_str_new_from_chars (
         path_and_query, question_mark - path_and_query);
      request->query = kms_request_str_new_from_chars (question_mark + 1, -1);
      request->query_params = parse_query_params (request->query);
      if (!request->query_params) {
         KMS_ERROR (request, "Cannot parse query: %s", request->query->str);
      }
   } else {
      request->path = kms_request_str_new_from_chars (path_and_query, -1);
      request->query = kms_request_str_new ();
      request->query_params = kms_kv_list_new ();
   }

   request->payload = kms_request_str_new ();
   request->date = kms_request_str_new ();
   request->datetime = kms_request_str_new ();
   request->method = kms_request_str_new_from_chars (method, -1);
   request->header_fields = kms_kv_list_new ();
   request->auto_content_length = true;

   /* For AWS KMS requests, add a X-Amz-Date header. */
   if (request->provider == KMS_REQUEST_PROVIDER_AWS &&
       !kms_request_set_date (request, NULL)) {
      return request;
   }

   if (opt && opt->connection_close) {
      if (!kms_request_add_header_field (request, "Connection", "close")) {
         return request;
      }
   }

   if (opt && opt->crypto.sha256) {
      memcpy (&request->crypto, &opt->crypto, sizeof (opt->crypto));
   } else {
      request->crypto.sha256 = kms_sha256;
      request->crypto.sha256_hmac = kms_sha256_hmac;
   }

   return request;
}

void
kms_request_destroy (kms_request_t *request)
{
   kms_request_str_destroy (request->region);
   kms_request_str_destroy (request->service);
   kms_request_str_destroy (request->access_key_id);
   kms_request_str_destroy (request->secret_key);
   kms_request_str_destroy (request->method);
   kms_request_str_destroy (request->path);
   kms_request_str_destroy (request->query);
   kms_request_str_destroy (request->payload);
   kms_request_str_destroy (request->datetime);
   kms_request_str_destroy (request->date);
   kms_kv_list_destroy (request->query_params);
   kms_kv_list_destroy (request->header_fields);
   kms_request_str_destroy (request->to_string);
   free (request->kmip.data);
   free (request);
}

const char *
kms_request_get_error (kms_request_t *request)
{
   return request->failed ? request->error : NULL;
}

#define AMZ_DT_FORMAT "YYYYmmDDTHHMMSSZ"

bool
kms_request_set_date (kms_request_t *request, const struct tm *tm)
{
   char buf[sizeof AMZ_DT_FORMAT];
   struct tm tmp_tm;

   if (request->failed) {
      return false;
   }

   if (!check_and_prohibit_kmip (request)) {
      return false;
   }

   if (!tm) {
      /* use current time */
      time_t t;
      time (&t);
#if defined(KMS_MESSAGE_HAVE_GMTIME_R)
      gmtime_r (&t, &tmp_tm);
#elif defined(_MSC_VER)
      gmtime_s (&tmp_tm, &t);
#else
      tmp_tm = *gmtime (&t);
#endif
      tm = &tmp_tm;
   }

   if (0 == strftime (buf, sizeof AMZ_DT_FORMAT, "%Y%m%dT%H%M%SZ", tm)) {
      KMS_ERROR (request, "Invalid tm struct");
      return false;
   }

   kms_request_str_set_chars (request->date, buf, sizeof "YYYYmmDD" - 1);
   kms_request_str_set_chars (request->datetime, buf, sizeof AMZ_DT_FORMAT - 1);
   kms_kv_list_del (request->header_fields, "X-Amz-Date");
   if (!kms_request_add_header_field (request, "X-Amz-Date", buf)) {
      return false;
   }

   return true;
}

#undef AMZ_DT_FORMAT

bool
kms_request_set_region (kms_request_t *request, const char *region)
{
   if (!check_and_prohibit_kmip (request)) {
      return false;
   }
   kms_request_str_set_chars (request->region, region, -1);
   return true;
}

bool
kms_request_set_service (kms_request_t *request, const char *service)
{
   if (!check_and_prohibit_kmip (request)) {
      return false;
   }
   kms_request_str_set_chars (request->service, service, -1);
   return true;
}

bool
kms_request_set_access_key_id (kms_request_t *request, const char *akid)
{
   if (!check_and_prohibit_kmip (request)) {
      return false;
   }
   kms_request_str_set_chars (request->access_key_id, akid, -1);
   return true;
}

bool
kms_request_set_secret_key (kms_request_t *request, const char *key)
{
   if (!check_and_prohibit_kmip (request)) {
      return false;
   }
   kms_request_str_set_chars (request->secret_key, key, -1);
   return true;
}

bool
kms_request_add_header_field (kms_request_t *request,
                              const char *field_name,
                              const char *value)
{
   kms_request_str_t *k, *v;

   CHECK_FAILED;

   if (!check_and_prohibit_kmip (request)) {
      return false;
   }

   k = kms_request_str_new_from_chars (field_name, -1);
   v = kms_request_str_new_from_chars (value, -1);
   kms_kv_list_add (request->header_fields, k, v);
   kms_request_str_destroy (k);
   kms_request_str_destroy (v);

   return true;
}

bool
kms_request_append_header_field_value (kms_request_t *request,
                                       const char *value,
                                       size_t len)
{
   kms_request_str_t *v;

   CHECK_FAILED;

   if (!check_and_prohibit_kmip (request)) {
      return false;
   }

   if (request->header_fields->len == 0) {
      KMS_ERROR (
         request,
         "Ensure the request has at least one header field before calling %s",
         __func__);
   }

   v = request->header_fields->kvs[request->header_fields->len - 1].value;
   KMS_ASSERT (len <= SSIZE_MAX);
   kms_request_str_append_chars (v, value, (ssize_t) len);

   return true;
}

bool
kms_request_append_payload (kms_request_t *request,
                            const char *payload,
                            size_t len)
{
   CHECK_FAILED;

   if (!check_and_prohibit_kmip (request)) {
      return false;
   }

   KMS_ASSERT (len <= SSIZE_MAX);
   kms_request_str_append_chars (request->payload, payload, (ssize_t) len);

   return true;
}

/* docs.aws.amazon.com/general/latest/gr/sigv4-create-canonical-request.html
 *
 * "Sort the parameter names by character code point in ascending order. For
 * example, a parameter name that begins with the uppercase letter F precedes a
 * parameter name that begins with a lowercase letter b."
 */
static int
cmp_query_params (const void *a, const void *b)
{
   int r = strcmp (((kms_kv_t *) a)->key->str, ((kms_kv_t *) b)->key->str);
   if (r != 0) {
      return r;
   }

   /* not in docs, but tested in get-vanilla-query-order-key: sort by value */
   return strcmp (((kms_kv_t *) a)->value->str, ((kms_kv_t *) b)->value->str);
}

static void
append_canonical_query (kms_request_t *request, kms_request_str_t *str)
{
   size_t i;
   kms_kv_list_t *lst;

   if (!request->query_params->len) {
      return;
   }

   lst = kms_kv_list_dup (request->query_params);
   kms_kv_list_sort (lst, cmp_query_params);

   for (i = 0; i < lst->len; i++) {
      kms_request_str_append_escaped (str, lst->kvs[i].key, true);
      kms_request_str_append_char (str, '=');
      kms_request_str_append_escaped (str, lst->kvs[i].value, true);

      if (i < lst->len - 1) {
         kms_request_str_append_char (str, '&');
      }
   }

   kms_kv_list_destroy (lst);
}

/* "lst" is a sorted list of headers */
static void
append_canonical_headers (kms_kv_list_t *lst, kms_request_str_t *str)
{
   size_t i;
   kms_kv_t *kv;
   const kms_request_str_t *previous_key = NULL;

   /* aws docs: "To create the canonical headers list, convert all header names
    * to lowercase and remove leading spaces and trailing spaces. Convert
    * sequential spaces in the header value to a single space." "Do not sort the
    * values in headers that have multiple values." */
   for (i = 0; i < lst->len; i++) {
      kv = &lst->kvs[i];
      if (previous_key &&
          0 == kms_strcasecmp (previous_key->str, kv->key->str)) {
         /* duplicate header */
         kms_request_str_append_char (str, ',');
         kms_request_str_append_stripped (str, kv->value);
         continue;
      }

      if (i > 0) {
         kms_request_str_append_newline (str);
      }

      kms_request_str_append_lowercase (str, kv->key);
      kms_request_str_append_char (str, ':');
      kms_request_str_append_stripped (str, kv->value);
      previous_key = kv->key;
   }

   kms_request_str_append_newline (str);
}

static void
append_signed_headers (kms_kv_list_t *lst, kms_request_str_t *str)
{
   size_t i;

   kms_kv_t *kv;
   const kms_request_str_t *previous_key = NULL;

   for (i = 0; i < lst->len; i++) {
      kv = &lst->kvs[i];
      if (previous_key &&
          0 == kms_strcasecmp (previous_key->str, kv->key->str)) {
         /* duplicate header */
         continue;
      }

      if (0 == kms_strcasecmp (kv->key->str, "connection")) {
         continue;
      }

      kms_request_str_append_lowercase (str, kv->key);
      if (i < lst->len - 1) {
         kms_request_str_append_char (str, ';');
      }

      previous_key = kv->key;
   }
}

static bool
finalize (kms_request_t *request)
{
   kms_kv_list_t *lst;
   kms_request_str_t *k;
   kms_request_str_t *v;

   if (request->failed) {
      return false;
   }

   if (request->finalized) {
      return true;
   }

   request->finalized = true;

   lst = request->header_fields;

   if (!kms_kv_list_find (lst, "Host")) {
      if (request->provider != KMS_REQUEST_PROVIDER_AWS) {
         KMS_ERROR (request, "Required Host header not set");
         return false;
      }
      /* For AWS requests, derive a default Host header from region + service.
       * E.g. "kms.us-east-1.amazonaws.com" */
      k = kms_request_str_new_from_chars ("Host", -1);
      v = kms_request_str_dup (request->service);
      kms_request_str_append_char (v, '.');
      kms_request_str_append (v, request->region);
      kms_request_str_append_chars (v, ".amazonaws.com", -1);
      kms_kv_list_add (lst, k, v);
      kms_request_str_destroy (k);
      kms_request_str_destroy (v);
   }

   if (!kms_kv_list_find (lst, "Content-Length") && request->payload->len &&
       request->auto_content_length) {
      k = kms_request_str_new_from_chars ("Content-Length", -1);
      v = kms_request_str_new ();
      kms_request_str_appendf (v, "%zu", request->payload->len);
      kms_kv_list_add (lst, k, v);
      kms_request_str_destroy (k);
      kms_request_str_destroy (v);
   }

   return true;
}

/* docs.aws.amazon.com/general/latest/gr/sigv4-create-canonical-request.html
 *
 * "Build the canonical headers list by sorting the (lowercase) headers by
 * character code... Do not sort the values in headers that have multiple
 * values."
 */
static int
cmp_header_field_names (const void *a, const void *b)
{
   return kms_strcasecmp (((kms_kv_t *) a)->key->str,
                          ((kms_kv_t *) b)->key->str);
}

static kms_kv_list_t *
canonical_headers (const kms_request_t *request)
{
   kms_kv_list_t *lst;

   KMS_ASSERT (request->finalized);
   lst = kms_kv_list_dup (request->header_fields);
   kms_kv_list_sort (lst, cmp_header_field_names);
   kms_kv_list_del (lst, "Connection");
   return lst;
}

char *
kms_request_get_canonical (kms_request_t *request)
{
   kms_request_str_t *canonical;
   kms_request_str_t *normalized;
   kms_kv_list_t *lst;

   if (request->failed) {
      return NULL;
   }

   if (!check_and_prohibit_kmip (request)) {
      return NULL;
   }

   if (!finalize (request)) {
      return NULL;
   }

   canonical = kms_request_str_new ();
   kms_request_str_append (canonical, request->method);
   kms_request_str_append_newline (canonical);
   normalized = kms_request_str_path_normalized (request->path);
   kms_request_str_append_escaped (canonical, normalized, false);
   kms_request_str_destroy (normalized);
   kms_request_str_append_newline (canonical);
   append_canonical_query (request, canonical);
   kms_request_str_append_newline (canonical);
   lst = canonical_headers (request);
   append_canonical_headers (lst, canonical);
   kms_request_str_append_newline (canonical);
   append_signed_headers (lst, canonical);
   kms_kv_list_destroy (lst);
   kms_request_str_append_newline (canonical);
   if (!kms_request_str_append_hashed (
          &request->crypto, canonical, request->payload)) {
      KMS_ERROR (request, "could not generate hash");
      kms_request_str_destroy (canonical);
      return NULL;
   }

   return kms_request_str_detach (canonical);
}

const char *
kms_request_get_canonical_header (kms_request_t *request, const char *header)
{
   const kms_kv_t *value;

   if (request->failed) {
      return NULL;
   }

   if (!check_and_prohibit_kmip (request)) {
      return NULL;
   }

   if (!finalize (request)) {
      return NULL;
   }

   value = kms_kv_list_find (request->header_fields, header);
   if (!value) {
      return NULL;
   }

   return value->value->str;
}

char *
kms_request_get_string_to_sign (kms_request_t *request)
{
   bool success = false;
   kms_request_str_t *sts;
   kms_request_str_t *creq = NULL; /* canonical request */

   if (request->failed) {
      return NULL;
   }

   if (!check_and_prohibit_kmip (request)) {
      return NULL;
   }

   if (!finalize (request)) {
      return NULL;
   }

   sts = kms_request_str_new ();
   kms_request_str_append_chars (sts, "AWS4-HMAC-SHA256\n", -1);
   kms_request_str_append (sts, request->datetime);
   kms_request_str_append_newline (sts);

   /* credential scope, like "20150830/us-east-1/service/aws4_request" */
   kms_request_str_append (sts, request->date);
   kms_request_str_append_char (sts, '/');
   kms_request_str_append (sts, request->region);
   kms_request_str_append_char (sts, '/');
   kms_request_str_append (sts, request->service);
   kms_request_str_append_chars (sts, "/aws4_request\n", -1);

   creq = kms_request_str_wrap (kms_request_get_canonical (request), -1);
   if (!creq) {
      goto done;
   }

   if (!kms_request_str_append_hashed (&request->crypto, sts, creq)) {
      goto done;
   }

   success = true;
done:
   kms_request_str_destroy (creq);
   if (!success) {
      kms_request_str_destroy (sts);
      sts = NULL;
   }

   return kms_request_str_detach (sts);
}

static bool
kms_request_hmac (_kms_crypto_t *crypto,
                  unsigned char *out,
                  kms_request_str_t *key,
                  kms_request_str_t *data)
{
   return crypto->sha256_hmac (
      crypto->ctx, key->str, key->len, data->str, data->len, out);
}

static bool
kms_request_hmac_again (_kms_crypto_t *crypto,
                        unsigned char *out,
                        unsigned char *in,
                        kms_request_str_t *data)
{
   return crypto->sha256_hmac (
      crypto->ctx, (const char *) in, 32, data->str, data->len, out);
}

bool
kms_request_get_signing_key (kms_request_t *request, unsigned char *key)
{
   bool success = false;
   kms_request_str_t *aws4_plus_secret = NULL;
   kms_request_str_t *aws4_request = NULL;
   unsigned char k_date[32];
   unsigned char k_region[32];
   unsigned char k_service[32];

   if (request->failed) {
      return false;
   }

   if (!check_and_prohibit_kmip (request)) {
      return false;
   }

   /* docs.aws.amazon.com/general/latest/gr/sigv4-calculate-signature.html
    * Pseudocode for deriving a signing key
    *
    * kSecret = your secret access key
    * kDate = HMAC("AWS4" + kSecret, Date)
    * kRegion = HMAC(kDate, Region)
    * kService = HMAC(kRegion, Service)
    * kSigning = HMAC(kService, "aws4_request")
    */
   aws4_plus_secret = kms_request_str_new_from_chars ("AWS4", -1);
   kms_request_str_append (aws4_plus_secret, request->secret_key);

   aws4_request = kms_request_str_new_from_chars ("aws4_request", -1);

   if (!(kms_request_hmac (
            &request->crypto, k_date, aws4_plus_secret, request->date) &&
         kms_request_hmac_again (
            &request->crypto, k_region, k_date, request->region) &&
         kms_request_hmac_again (
            &request->crypto, k_service, k_region, request->service) &&
         kms_request_hmac_again (
            &request->crypto, key, k_service, aws4_request))) {
      goto done;
   }

   success = true;
done:
   kms_request_str_destroy (aws4_plus_secret);
   kms_request_str_destroy (aws4_request);

   return success;
}

char *
kms_request_get_signature (kms_request_t *request)
{
   bool success = false;
   kms_kv_list_t *lst = NULL;
   kms_request_str_t *sig = NULL;
   kms_request_str_t *sts = NULL;
   unsigned char signing_key[32];
   unsigned char signature[32];

   if (request->failed) {
      return NULL;
   }

   if (!check_and_prohibit_kmip (request)) {
      return NULL;
   }

   sts = kms_request_str_wrap (kms_request_get_string_to_sign (request), -1);
   if (!sts) {
      goto done;
   }

   sig = kms_request_str_new ();
   kms_request_str_append_chars (sig, "AWS4-HMAC-SHA256 Credential=", -1);
   kms_request_str_append (sig, request->access_key_id);
   kms_request_str_append_char (sig, '/');
   kms_request_str_append (sig, request->date);
   kms_request_str_append_char (sig, '/');
   kms_request_str_append (sig, request->region);
   kms_request_str_append_char (sig, '/');
   kms_request_str_append (sig, request->service);
   kms_request_str_append_chars (sig, "/aws4_request, SignedHeaders=", -1);
   lst = canonical_headers (request);
   append_signed_headers (lst, sig);
   kms_request_str_append_chars (sig, ", Signature=", -1);
   if (!(kms_request_get_signing_key (request, signing_key) &&
         kms_request_hmac_again (
            &request->crypto, signature, signing_key, sts))) {
      goto done;
   }

   kms_request_str_append_hex (sig, signature, sizeof (signature));
   success = true;
done:
   kms_kv_list_destroy (lst);
   kms_request_str_destroy (sts);

   if (!success) {
      kms_request_str_destroy (sig);
      sig = NULL;
   }

   return kms_request_str_detach (sig);
}

void
kms_request_validate (kms_request_t *request)
{
   if (!check_and_prohibit_kmip (request)) {
      return;
   }
   if (0 == request->region->len) {
      KMS_ERROR (request, "Region not set");
   } else if (0 == request->service->len) {
      KMS_ERROR (request, "Service not set");
   } else if (0 == request->access_key_id->len) {
      KMS_ERROR (request, "Access key ID not set");
   } else if (0 == request->method->len) {
      KMS_ERROR (request, "Method not set");
   } else if (0 == request->path->len) {
      KMS_ERROR (request, "Path not set");
   } else if (0 == request->date->len) {
      KMS_ERROR (request, "Date not set");
   } else if (0 == request->secret_key->len) {
      KMS_ERROR (request, "Secret key not set");
   }
}

/* append_http_endofline appends an HTTP end-of-line marker: "\r\n". */
static void
append_http_endofline (kms_request_str_t *str)
{
   kms_request_str_append_chars (str, "\r\n", 2);
}

char *
kms_request_get_signed (kms_request_t *request)
{
   bool success = false;
   kms_kv_list_t *lst = NULL;
   char *signature = NULL;
   kms_request_str_t *sreq = NULL;
   size_t i;

   kms_request_validate (request);
   if (request->failed) {
      return NULL;
   }

   if (!check_and_prohibit_kmip (request)) {
      return false;
   }

   if (!finalize (request)) {
      return NULL;
   }

   sreq = kms_request_str_new ();
   /* like "POST / HTTP/1.1" */
   kms_request_str_append (sreq, request->method);
   kms_request_str_append_char (sreq, ' ');
   kms_request_str_append (sreq, request->path);
   if (request->query->len) {
      kms_request_str_append_char (sreq, '?');
      kms_request_str_append (sreq, request->query);
   }

   kms_request_str_append_chars (sreq, " HTTP/1.1", -1);
   append_http_endofline (sreq);

   /* headers */
   lst = kms_kv_list_dup (request->header_fields);
   kms_kv_list_sort (lst, cmp_header_field_names);
   for (i = 0; i < lst->len; i++) {
      kms_request_str_append (sreq, lst->kvs[i].key);
      kms_request_str_append_char (sreq, ':');
      kms_request_str_append (sreq, lst->kvs[i].value);
      append_http_endofline (sreq);
   }

   /* authorization header */
   signature = kms_request_get_signature (request);
   if (!signature) {
      goto done;
   }

   /* note space after ':', to match test .sreq files */
   kms_request_str_append_chars (sreq, "Authorization: ", -1);
   kms_request_str_append_chars (sreq, signature, -1);

   /* body */
   if (request->payload->len) {
      append_http_endofline (sreq);
      append_http_endofline (sreq);
      kms_request_str_append (sreq, request->payload);
   }

   success = true;
done:
   free (signature);
   kms_kv_list_destroy (lst);

   if (!success) {
      kms_request_str_destroy (sreq);
      sreq = NULL;
   }

   return kms_request_str_detach (sreq);
}

char *
kms_request_to_string (kms_request_t *request)
{
   kms_kv_list_t *lst = NULL;
   kms_request_str_t *sreq = NULL;
   size_t i;

   if (!finalize (request)) {
      return false;
   }

   if (!check_and_prohibit_kmip (request)) {
      return false;
   }

   if (request->to_string) {
      return kms_request_str_detach (kms_request_str_dup (request->to_string));
   }

   sreq = kms_request_str_new ();
   /* like "POST / HTTP/1.1" */
   kms_request_str_append (sreq, request->method);
   kms_request_str_append_char (sreq, ' ');
   kms_request_str_append (sreq, request->path);
   if (request->query->len) {
      kms_request_str_append_char (sreq, '?');
      kms_request_str_append (sreq, request->query);
   }

   kms_request_str_append_chars (sreq, " HTTP/1.1", -1);
   append_http_endofline (sreq);

   /* headers */
   lst = kms_kv_list_dup (request->header_fields);
   kms_kv_list_sort (lst, cmp_header_field_names);
   for (i = 0; i < lst->len; i++) {
      kms_request_str_append (sreq, lst->kvs[i].key);
      kms_request_str_append_char (sreq, ':');
      kms_request_str_append (sreq, lst->kvs[i].value);
      append_http_endofline (sreq);
   }

   append_http_endofline (sreq);

   /* body */
   if (request->payload->len) {
      kms_request_str_append (sreq, request->payload);
   }

   kms_kv_list_destroy (lst);
   request->to_string = kms_request_str_dup (sreq);
   return kms_request_str_detach (sreq);
}

void
kms_request_free_string (char *ptr)
{
   free (ptr);
}

const uint8_t *
kms_request_to_bytes (kms_request_t *request, size_t *len)
{
   if (request->provider == KMS_REQUEST_PROVIDER_KMIP) {
      *len = request->kmip.len;
      return request->kmip.data;
   }

   if (!request->to_string && !kms_request_to_string (request)) {
      return NULL;
   }

   KMS_ASSERT (request->to_string);
   *len = request->to_string->len;
   return (const uint8_t*) request->to_string->str;
}
