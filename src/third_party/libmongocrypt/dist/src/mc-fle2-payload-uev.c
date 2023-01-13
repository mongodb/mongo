/*
 * Copyright 2022-present MongoDB, Inc.
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

#include "mc-fle2-payload-uev-private.h"
#include "mongocrypt-private.h"
#include "mc-fle-blob-subtype-private.h"

struct _mc_FLE2UnindexedEncryptedValue_t {
   _mongocrypt_buffer_t key_uuid;
   uint8_t original_bson_type;
   _mongocrypt_buffer_t ciphertext;
   _mongocrypt_buffer_t plaintext;
   bool parsed;
};

mc_FLE2UnindexedEncryptedValue_t *
mc_FLE2UnindexedEncryptedValue_new (void)
{
   mc_FLE2UnindexedEncryptedValue_t *uev =
      bson_malloc0 (sizeof (mc_FLE2UnindexedEncryptedValue_t));
   return uev;
}

bool
mc_FLE2UnindexedEncryptedValue_parse (mc_FLE2UnindexedEncryptedValue_t *uev,
                                      const _mongocrypt_buffer_t *buf,
                                      mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (uev);
   BSON_ASSERT_PARAM (buf);

   if (uev->parsed) {
      CLIENT_ERR (
         "mc_FLE2UnindexedEncryptedValue_parse must not be called twice");
      return false;
   }

   uint32_t offset = 0;
   /* Read fle_blob_subtype. */
   if (offset + 1 > buf->len) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_parse expected byte "
                  "length >= %" PRIu32 " got: %" PRIu32,
                  offset + 1,
                  buf->len);
      return false;
   }

   uint8_t fle_blob_subtype = buf->data[offset];
   if (fle_blob_subtype != MC_SUBTYPE_FLE2UnindexedEncryptedValue) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_parse expected "
                  "fle_blob_subtype=%d got: %" PRIu8,
                  MC_SUBTYPE_FLE2UnindexedEncryptedValue,
                  fle_blob_subtype);
      return false;
   }
   offset += 1;

   /* Read key_uuid. */
   if (offset + 16 > buf->len) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_parse expected byte "
                  "length >= %" PRIu32 " got: %" PRIu32,
                  offset + 16,
                  buf->len);
      return false;
   }
   if (!_mongocrypt_buffer_copy_from_data_and_size (
          &uev->key_uuid, buf->data + offset, 16)) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_parse failed to copy "
                  "data for key_uuid");
      return false;
   }
   uev->key_uuid.subtype = BSON_SUBTYPE_UUID;
   offset += 16;

   /* Read original_bson_type. */
   if (offset + 1 > buf->len) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_parse expected byte "
                  "length >= %" PRIu32 " got: %" PRIu32,
                  offset + 1,
                  buf->len);
      return false;
   }
   uev->original_bson_type = buf->data[offset];
   offset += 1;

   /* Read ciphertext. */
   if (!_mongocrypt_buffer_copy_from_data_and_size (
          &uev->ciphertext, buf->data + offset, (size_t) (buf->len - offset))) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_parse failed to copy "
                  "data for ciphertext");
      return false;
   }

   uev->parsed = true;
   return true;
}

bson_type_t
mc_FLE2UnindexedEncryptedValue_get_original_bson_type (
   const mc_FLE2UnindexedEncryptedValue_t *uev, mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (uev);

   if (!uev->parsed) {
      CLIENT_ERR (
         "mc_FLE2UnindexedEncryptedValue_get_original_bson_type must be "
         "called after mc_FLE2UnindexedEncryptedValue_parse");
      return 0;
   }
   return uev->original_bson_type;
}

const _mongocrypt_buffer_t *
mc_FLE2UnindexedEncryptedValue_get_key_uuid (
   const mc_FLE2UnindexedEncryptedValue_t *uev, mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (uev);

   if (!uev->parsed) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_get_key_uuid must be "
                  "called after mc_FLE2UnindexedEncryptedValue_parse");
      return NULL;
   }
   return &uev->key_uuid;
}

const _mongocrypt_buffer_t *
mc_FLE2UnindexedEncryptedValue_decrypt (_mongocrypt_crypto_t *crypto,
                                        mc_FLE2UnindexedEncryptedValue_t *uev,
                                        const _mongocrypt_buffer_t *key,
                                        mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (uev);
   BSON_ASSERT_PARAM (key);

   if (!uev->parsed) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_decrypt must be "
                  "called after mc_FLE2UnindexedEncryptedValue_parse");
      return NULL;
   }

   /* Serialize associated data: fle_blob_subtype || key_uuid ||
    * original_bson_type */
   _mongocrypt_buffer_t AD;
   _mongocrypt_buffer_init (&AD);
   if (uev->key_uuid.len > UINT32_MAX - 2) {
      CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_decrypt expected "
                  "key UUID length <= %" PRIu32 " got: %" PRIu32,
                  UINT32_MAX - 2u,
                  uev->key_uuid.len);
      return NULL;
   }
   _mongocrypt_buffer_resize (&AD, 1 + uev->key_uuid.len + 1);

   AD.data[0] = MC_SUBTYPE_FLE2UnindexedEncryptedValue;
   memcpy (AD.data + 1, uev->key_uuid.data, uev->key_uuid.len);
   AD.data[1 + uev->key_uuid.len] = uev->original_bson_type;
   const uint32_t plaintext_len = _mongocrypt_fle2aead_calculate_plaintext_len (
      uev->ciphertext.len, status);
   if (plaintext_len == 0) {
      return NULL;
   }
   _mongocrypt_buffer_resize (&uev->plaintext, plaintext_len);

   uint32_t bytes_written;

   if (!_mongocrypt_fle2aead_do_decryption (crypto,
                                            &AD,
                                            key,
                                            &uev->ciphertext,
                                            &uev->plaintext,
                                            &bytes_written,
                                            status)) {
      _mongocrypt_buffer_cleanup (&AD);
      return NULL;
   }

   _mongocrypt_buffer_cleanup (&AD);
   return &uev->plaintext;
}

bool
mc_FLE2UnindexedEncryptedValue_encrypt (_mongocrypt_crypto_t *crypto,
                                        const _mongocrypt_buffer_t *key_uuid,
                                        bson_type_t original_bson_type,
                                        const _mongocrypt_buffer_t *plaintext,
                                        const _mongocrypt_buffer_t *key,
                                        _mongocrypt_buffer_t *out,
                                        mongocrypt_status_t *status)
{
   _mongocrypt_buffer_t iv = {0};
   _mongocrypt_buffer_t AD = {0};
   bool res = false;

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (key_uuid);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (out);

   _mongocrypt_buffer_resize (&iv, MONGOCRYPT_IV_LEN);
   if (!_mongocrypt_random (crypto, &iv, MONGOCRYPT_IV_LEN, status)) {
      goto fail;
   }

   /* Serialize associated data: fle_blob_subtype || key_uuid ||
    * original_bson_type */
   {
      if (key_uuid->len > UINT32_MAX - 2) {
         CLIENT_ERR ("mc_FLE2UnindexedEncryptedValue_encrypt expected "
                     "key UUID length <= %" PRIu32 " got: %" PRIu32,
                     UINT32_MAX - 2u,
                     key_uuid->len);
         goto fail;
      }
      _mongocrypt_buffer_resize (&AD, 1 + key_uuid->len + 1);
      AD.data[0] = MC_SUBTYPE_FLE2UnindexedEncryptedValue;
      memcpy (AD.data + 1, key_uuid->data, key_uuid->len);
      AD.data[1 + key_uuid->len] = (uint8_t) original_bson_type;
   }

   /* Encrypt. */
   {
      const uint32_t cipherlen =
         _mongocrypt_fle2aead_calculate_ciphertext_len (plaintext->len, status);
      if (cipherlen == 0) {
         goto fail;
      }
      _mongocrypt_buffer_resize (out, cipherlen);
      uint32_t bytes_written; /* unused. */
      if (!_mongocrypt_fle2aead_do_encryption (
             crypto, &iv, &AD, key, plaintext, out, &bytes_written, status)) {
         goto fail;
      }
   }

   res = true;
fail:
   _mongocrypt_buffer_cleanup (&AD);
   _mongocrypt_buffer_cleanup (&iv);
   return res;
}

void
mc_FLE2UnindexedEncryptedValue_destroy (mc_FLE2UnindexedEncryptedValue_t *uev)
{
   if (NULL == uev) {
      return;
   }
   _mongocrypt_buffer_cleanup (&uev->key_uuid);
   _mongocrypt_buffer_cleanup (&uev->ciphertext);
   _mongocrypt_buffer_cleanup (&uev->plaintext);

   bson_free (uev);
}
