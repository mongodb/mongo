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

/*
 * Comments in this implementation refer to:
 * [MCGREW] https://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-05
 */

#include <bson/bson.h>

#include "mongocrypt-binary-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-log-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-status-private.h"

#include <inttypes.h>

/* This function uses ECB callback to simulate CTR encrypt and decrypt
 *
 * Note: the same function performs both encrypt and decrypt using same ECB
 * encryption function
 */

static bool
_crypto_aes_256_ctr_encrypt_decrypt_via_ecb (
   void *ctx,
   mongocrypt_crypto_fn aes_256_ecb_encrypt,
   aes_256_args_t args,
   mongocrypt_status_t *status)
{
   BSON_ASSERT (args.iv && args.iv->len);
   BSON_ASSERT (args.in);
   BSON_ASSERT (args.out);

   if (args.out->len < args.in->len) {
      CLIENT_ERR ("output buffer too small");
      return false;
   }

   _mongocrypt_buffer_t ctr, tmp;
   mongocrypt_binary_t key_bin, out_bin, in_bin, ctr_bin, tmp_bin;
   bool ret;

   _mongocrypt_buffer_to_binary (args.key, &key_bin);
   _mongocrypt_buffer_init (&ctr);
   _mongocrypt_buffer_copy_to (args.iv, &ctr);
   _mongocrypt_buffer_to_binary (&ctr, &ctr_bin);
   _mongocrypt_buffer_to_binary (args.out, &out_bin);
   _mongocrypt_buffer_to_binary (args.in, &in_bin);
   _mongocrypt_buffer_init_size (&tmp, args.iv->len);
   _mongocrypt_buffer_to_binary (&tmp, &tmp_bin);

   for (uint32_t ptr = 0; ptr < args.in->len;) {
      /* Encrypt value in CTR buffer */
      uint32_t bytes_written = 0;
      if (!aes_256_ecb_encrypt (
             ctx, &key_bin, NULL, &ctr_bin, &tmp_bin, &bytes_written, status)) {
         ret = false;
         goto cleanup;
      }

      if (bytes_written != tmp_bin.len) {
         CLIENT_ERR ("encryption hook returned unexpected length");
         ret = false;
         goto cleanup;
      }

      /* XOR resulting stream with original data */
      for (uint32_t i = 0; i < bytes_written && ptr < args.in->len;
           i++, ptr++) {
         out_bin.data[ptr] = in_bin.data[ptr] ^ tmp_bin.data[i];
      }

      /* Increment value in CTR buffer */
      uint32_t carry = 1;
      /* assert rather than return since this should never happen */
      BSON_ASSERT (ctr_bin.len == 0u || ctr_bin.len - 1u <= INT_MAX);
      for (int i = (int) ctr_bin.len - 1; i >= 0 && carry != 0; --i) {
         uint32_t bpp = carry + ctr_bin.data[i];
         carry = bpp >> 8;
         ctr_bin.data[i] = bpp & 0xFF;
      }
   }

   if (args.bytes_written) {
      *args.bytes_written = args.in->len;
   }

   ret = true;

cleanup:
   _mongocrypt_buffer_cleanup (&ctr);
   _mongocrypt_buffer_cleanup (&tmp);
   return ret;
}

/* Crypto primitives. These either call the native built in crypto primitives or
 * user supplied hooks. */
static bool
_crypto_aes_256_cbc_encrypt (_mongocrypt_crypto_t *crypto, aes_256_args_t args)
{
   mongocrypt_status_t *status = args.status;

   BSON_ASSERT_PARAM (crypto);

   BSON_ASSERT (args.key);
   if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
      CLIENT_ERR ("invalid encryption key length");
      return false;
   }

   BSON_ASSERT (args.iv);
   if (args.iv->len != MONGOCRYPT_IV_LEN) {
      CLIENT_ERR ("invalid iv length");
      return false;
   }

   if (crypto->hooks_enabled) {
      mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
      bool ret;

      _mongocrypt_buffer_to_binary (args.key, &enc_key_bin);
      _mongocrypt_buffer_to_binary (args.iv, &iv_bin);
      _mongocrypt_buffer_to_binary (args.out, &out_bin);
      _mongocrypt_buffer_to_binary (args.in, &in_bin);

      ret = crypto->aes_256_cbc_encrypt (crypto->ctx,
                                         &enc_key_bin,
                                         &iv_bin,
                                         &in_bin,
                                         &out_bin,
                                         args.bytes_written,
                                         status);
      return ret;
   }
   return _native_crypto_aes_256_cbc_encrypt (args);
}

static bool
_crypto_aes_256_ctr_encrypt (_mongocrypt_crypto_t *crypto, aes_256_args_t args)
{
   mongocrypt_status_t *status = args.status;

   BSON_ASSERT_PARAM (crypto);

   BSON_ASSERT (args.key);
   if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
      CLIENT_ERR ("invalid encryption key length");
      return false;
   }

   BSON_ASSERT (args.iv);
   if (args.iv->len != MONGOCRYPT_IV_LEN) {
      CLIENT_ERR ("invalid iv length");
      return false;
   }

   if (crypto->aes_256_ctr_encrypt) {
      mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
      bool ret;

      _mongocrypt_buffer_to_binary (args.key, &enc_key_bin);
      _mongocrypt_buffer_to_binary (args.iv, &iv_bin);
      _mongocrypt_buffer_to_binary (args.out, &out_bin);
      _mongocrypt_buffer_to_binary (args.in, &in_bin);

      ret = crypto->aes_256_ctr_encrypt (crypto->ctx,
                                         &enc_key_bin,
                                         &iv_bin,
                                         &in_bin,
                                         &out_bin,
                                         args.bytes_written,
                                         status);
      return ret;
   }

   if (crypto->aes_256_ecb_encrypt) {
      return _crypto_aes_256_ctr_encrypt_decrypt_via_ecb (
         crypto->ctx, crypto->aes_256_ecb_encrypt, args, status);
   }

   return _native_crypto_aes_256_ctr_encrypt (args);
}

static bool
_crypto_aes_256_cbc_decrypt (_mongocrypt_crypto_t *crypto, aes_256_args_t args)
{
   mongocrypt_status_t *status = args.status;

   BSON_ASSERT_PARAM (crypto);

   BSON_ASSERT (args.key);
   if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
      CLIENT_ERR ("invalid encryption key length");
      return false;
   }

   if (crypto->hooks_enabled) {
      mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
      bool ret;

      _mongocrypt_buffer_to_binary (args.key, &enc_key_bin);
      _mongocrypt_buffer_to_binary (args.iv, &iv_bin);
      _mongocrypt_buffer_to_binary (args.out, &out_bin);
      _mongocrypt_buffer_to_binary (args.in, &in_bin);

      ret = crypto->aes_256_cbc_decrypt (crypto->ctx,
                                         &enc_key_bin,
                                         &iv_bin,
                                         &in_bin,
                                         &out_bin,
                                         args.bytes_written,
                                         status);
      return ret;
   }
   return _native_crypto_aes_256_cbc_decrypt (args);
}

static bool
_crypto_aes_256_ctr_decrypt (_mongocrypt_crypto_t *crypto, aes_256_args_t args)
{
   mongocrypt_status_t *status = args.status;

   BSON_ASSERT_PARAM (crypto);

   BSON_ASSERT (args.key);
   if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
      CLIENT_ERR ("invalid encryption key length");
      return false;
   }

   if (crypto->aes_256_ctr_decrypt) {
      mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
      bool ret;

      _mongocrypt_buffer_to_binary (args.key, &enc_key_bin);
      _mongocrypt_buffer_to_binary (args.iv, &iv_bin);
      _mongocrypt_buffer_to_binary (args.out, &out_bin);
      _mongocrypt_buffer_to_binary (args.in, &in_bin);

      ret = crypto->aes_256_ctr_decrypt (crypto->ctx,
                                         &enc_key_bin,
                                         &iv_bin,
                                         &in_bin,
                                         &out_bin,
                                         args.bytes_written,
                                         status);
      return ret;
   }

   if (crypto->aes_256_ecb_encrypt) {
      return _crypto_aes_256_ctr_encrypt_decrypt_via_ecb (
         crypto->ctx, crypto->aes_256_ecb_encrypt, args, status);
   }

   return _native_crypto_aes_256_ctr_decrypt (args);
}

static bool
_crypto_hmac_sha_512 (_mongocrypt_crypto_t *crypto,
                      const _mongocrypt_buffer_t *hmac_key,
                      const _mongocrypt_buffer_t *in,
                      _mongocrypt_buffer_t *out,
                      mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (hmac_key);
   BSON_ASSERT_PARAM (in);
   BSON_ASSERT_PARAM (out);

   if (hmac_key->len != MONGOCRYPT_MAC_KEY_LEN) {
      CLIENT_ERR ("invalid hmac key length");
      return false;
   }

   if (out->len != MONGOCRYPT_HMAC_SHA512_LEN) {
      CLIENT_ERR ("out does not contain %d bytes", MONGOCRYPT_HMAC_SHA512_LEN);
      return false;
   }

   if (crypto->hooks_enabled) {
      mongocrypt_binary_t hmac_key_bin, out_bin, in_bin;
      bool ret;

      _mongocrypt_buffer_to_binary (hmac_key, &hmac_key_bin);
      _mongocrypt_buffer_to_binary (out, &out_bin);
      _mongocrypt_buffer_to_binary (in, &in_bin);

      ret = crypto->hmac_sha_512 (
         crypto->ctx, &hmac_key_bin, &in_bin, &out_bin, status);
      return ret;
   }
   return _native_crypto_hmac_sha_512 (hmac_key, in, out, status);
}


static bool
_crypto_random (_mongocrypt_crypto_t *crypto,
                _mongocrypt_buffer_t *out,
                uint32_t count,
                mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (out);

   if (out->len != count) {
      CLIENT_ERR ("out does not contain %u bytes", count);
      return false;
   }

   if (crypto->hooks_enabled) {
      mongocrypt_binary_t out_bin;

      _mongocrypt_buffer_to_binary (out, &out_bin);
      return crypto->random (crypto->ctx, &out_bin, count, status);
   }
   return _native_crypto_random (out, count, status);
}


/*
 * Secure memcmp copied from the C driver.
 */
int
_mongocrypt_memequal (const void *const b1, const void *const b2, size_t len)
{
   const unsigned char *p1 = b1, *p2 = b2;
   int ret = 0;

   BSON_ASSERT_PARAM (b1);
   BSON_ASSERT_PARAM (b2);

   for (; len > 0; len--) {
      ret |= *p1++ ^ *p2++;
   }

   return ret;
}

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_calculate_ciphertext_len --
 *
 *    For a given plaintext length, return the length of the ciphertext.
 *    This includes IV and HMAC.
 *
 *    To compute that I'm following section 2.3 in [MCGREW]:
 *    L = 16 * ( floor(M / 16) + 2)
 *    This formula includes space for the IV, but not the sha512 HMAC.
 *    Add 32 for the sha512 HMAC.
 *
 * Parameters:
 *    @plaintext_len then length of the plaintext.
 *    @status set on error.
 *
 * Returns:
 *    The calculated length of the ciphertext.
 *
 * ----------------------------------------------------------------------------
 */
uint32_t
_mongocrypt_calculate_ciphertext_len (uint32_t plaintext_len,
                                      mongocrypt_status_t *status)
{
   if ((plaintext_len / 16u) >
       ((UINT32_MAX - (uint32_t) MONGOCRYPT_HMAC_LEN) / 16u) - 2u) {
      CLIENT_ERR ("plaintext too long");
      return 0;
   }
   return 16 * ((plaintext_len / 16) + 2) + MONGOCRYPT_HMAC_LEN;
}

uint32_t
_mongocrypt_fle2aead_calculate_ciphertext_len (uint32_t plaintext_len,
                                               mongocrypt_status_t *status)
{
   if (plaintext_len > UINT32_MAX - MONGOCRYPT_IV_LEN - MONGOCRYPT_HMAC_LEN) {
      CLIENT_ERR ("plaintext too long");
      return 0;
   }
   /* FLE2 AEAD uses CTR mode. CTR mode does not pad. */
   return MONGOCRYPT_IV_LEN + plaintext_len + MONGOCRYPT_HMAC_LEN;
}

uint32_t
_mongocrypt_fle2_calculate_ciphertext_len (uint32_t plaintext_len,
                                           mongocrypt_status_t *status)
{
   if (plaintext_len > UINT32_MAX - MONGOCRYPT_IV_LEN) {
      CLIENT_ERR ("plaintext too long");
      return 0;
   }
   /* FLE2 AEAD uses CTR mode. CTR mode does not pad. */
   return MONGOCRYPT_IV_LEN + plaintext_len;
}


/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_calculate_plaintext_len --
 *
 *    For a given ciphertext length, return the length of the plaintext.
 *    This excludes the IV and HMAC, but includes the padding.
 *
 * Parameters:
 *    @ciphertext_len then length of the ciphertext.
 *    @status set on error.
 *
 * Returns:
 *    The calculated length of the plaintext.
 *
 * ----------------------------------------------------------------------------
 */
uint32_t
_mongocrypt_calculate_plaintext_len (uint32_t ciphertext_len,
                                     mongocrypt_status_t *status)
{
   if (ciphertext_len <
       MONGOCRYPT_HMAC_LEN + MONGOCRYPT_IV_LEN + MONGOCRYPT_BLOCK_SIZE) {
      CLIENT_ERR ("ciphertext too short");
      return 0;
   }
   return ciphertext_len - (MONGOCRYPT_IV_LEN + MONGOCRYPT_HMAC_LEN);
}

uint32_t
_mongocrypt_fle2aead_calculate_plaintext_len (uint32_t ciphertext_len,
                                              mongocrypt_status_t *status)
{
   /* FLE2 AEAD uses CTR mode. CTR mode does not pad. */
   if (ciphertext_len < MONGOCRYPT_IV_LEN + MONGOCRYPT_HMAC_LEN) {
      CLIENT_ERR ("ciphertext too short");
      return 0;
   }
   return ciphertext_len - MONGOCRYPT_IV_LEN - MONGOCRYPT_HMAC_LEN;
}

uint32_t
_mongocrypt_fle2_calculate_plaintext_len (uint32_t ciphertext_len,
                                          mongocrypt_status_t *status)
{
   /* FLE2 AEAD uses CTR mode. CTR mode does not pad. */
   if (ciphertext_len < MONGOCRYPT_IV_LEN) {
      CLIENT_ERR ("ciphertext too short");
      return 0;
   }
   return ciphertext_len - MONGOCRYPT_IV_LEN;
}

/* ----------------------------------------------------------------------------
 *
 * _aes256_cbc_encrypt --
 *
 *    Encrypts using AES256 CBC using a secret key and a known IV.
 *
 * Parameters:
 *    @iv a 16 byte IV.
 *    @enc_key a 32 byte key.
 *    @plaintext the plaintext to encrypt.
 *    @ciphertext the resulting ciphertext.
 *    @bytes_written a location for the resulting number of bytes written into
 *    ciphertext->data.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. ciphertext->data has been pre-allocated with enough space for the
 *    resulting ciphertext.
 *
 * Postconditions:
 *    1. bytes_written is set to the length of the written ciphertext. This
 *    is the same as
 *    _mongocrypt_calculate_ciphertext_len (plaintext->len, status).
 *
 * ----------------------------------------------------------------------------
 */
static bool
_encrypt_step (_mongocrypt_crypto_t *crypto,
               const _mongocrypt_buffer_t *iv,
               const _mongocrypt_buffer_t *enc_key,
               const _mongocrypt_buffer_t *plaintext,
               _mongocrypt_buffer_t *ciphertext,
               uint32_t *bytes_written,
               mongocrypt_status_t *status)
{
   uint32_t unaligned;
   uint32_t padding_byte;
   _mongocrypt_buffer_t intermediates[2];
   _mongocrypt_buffer_t to_encrypt;
   uint8_t final_block_storage[MONGOCRYPT_BLOCK_SIZE];
   bool ret = false;

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (iv);
   BSON_ASSERT_PARAM (enc_key);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (ciphertext);

   _mongocrypt_buffer_init (&to_encrypt);

   BSON_ASSERT_PARAM (bytes_written);
   *bytes_written = 0;

   if (MONGOCRYPT_IV_LEN != iv->len) {
      CLIENT_ERR ("IV should have length %d, but has length %d",
                  MONGOCRYPT_IV_LEN,
                  iv->len);
      goto done;
   }

   if (MONGOCRYPT_ENC_KEY_LEN != enc_key->len) {
      CLIENT_ERR ("Encryption key should have length %d, but has length %d",
                  MONGOCRYPT_ENC_KEY_LEN,
                  enc_key->len);
      goto done;
   }

   /* calculate how many extra bytes there are after a block boundary */
   unaligned = plaintext->len % MONGOCRYPT_BLOCK_SIZE;

   /* Some crypto providers disallow variable length inputs, and require
    * the input to be a multiple of the block size. So add everything up
    * to but excluding the last block if not block aligned, then add
    * the last block with padding. */
   _mongocrypt_buffer_init (&intermediates[0]);
   _mongocrypt_buffer_init (&intermediates[1]);
   intermediates[0].data = (uint8_t *) plaintext->data;
   /* don't check plaintext->len, as the above modulo operation guarantees
    * that unaligned will be smaller */
   intermediates[0].len = plaintext->len - unaligned;
   intermediates[1].data = final_block_storage;
   intermediates[1].len = sizeof (final_block_storage);

   /* [MCGREW]: "Prior to CBC encryption, the plaintext P is padded by appending
    * a padding string PS to that data, to ensure that len(P || PS) is a
    * multiple of 128". This is also known as PKCS #7 padding. */
   if (unaligned) {
      /* Copy the unaligned bytes. */
      memcpy (intermediates[1].data,
              plaintext->data + (plaintext->len - unaligned),
              unaligned);
      /* Fill the rest with the padding byte. */
      BSON_ASSERT (MONGOCRYPT_BLOCK_SIZE >= unaligned);
      padding_byte = MONGOCRYPT_BLOCK_SIZE - unaligned;
      /* it is certain that padding_byte is in range for a cast to int */
      memset (
         intermediates[1].data + unaligned, (int) padding_byte, padding_byte);
   } else {
      /* Fill the rest with the padding byte. */
      padding_byte = MONGOCRYPT_BLOCK_SIZE;
      memset (intermediates[1].data, (int) padding_byte, padding_byte);
   }

   if (!_mongocrypt_buffer_concat (&to_encrypt, intermediates, 2)) {
      CLIENT_ERR ("failed to allocate buffer");
      goto done;
   }

   if (!_crypto_aes_256_cbc_encrypt (
          crypto,
          (aes_256_args_t){.key = enc_key,
                           .iv = iv,
                           .in = &to_encrypt,
                           .out = ciphertext,
                           .bytes_written = bytes_written,
                           .status = status})) {
      goto done;
   }


   if (*bytes_written % MONGOCRYPT_BLOCK_SIZE != 0) {
      CLIENT_ERR ("encryption failure, wrote %d bytes, not a multiple of %d",
                  *bytes_written,
                  MONGOCRYPT_BLOCK_SIZE);
      goto done;
   }

   ret = true;
done:
   _mongocrypt_buffer_cleanup (&to_encrypt);
   return ret;
}


/* ----------------------------------------------------------------------------
 *
 * _hmac_sha512 --
 *
 *    Compute the SHA512 HMAC with a secret key.
 *
 * Parameters:
 *    @mac_key a 32 byte key.
 *    @associated_data associated data to add into the HMAC. This may be
 *    an empty buffer.
 *    @ciphertext the ciphertext to add into the HMAC.
 *    @out a location for the resulting HMAC tag.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. out->data has been pre-allocated with at least 64 bytes.
 *
 * Postconditions:
 *    1. out->data will have a 64 byte tag appended.
 *
 * ----------------------------------------------------------------------------
 */
static bool
_hmac_step (_mongocrypt_crypto_t *crypto,
            const _mongocrypt_buffer_t *mac_key,
            const _mongocrypt_buffer_t *associated_data,
            const _mongocrypt_buffer_t *ciphertext,
            _mongocrypt_buffer_t *out,
            mongocrypt_status_t *status)
{
   _mongocrypt_buffer_t intermediates[3];
   _mongocrypt_buffer_t to_hmac;
   uint64_t associated_data_len_be;
   uint8_t tag_storage[64];
   _mongocrypt_buffer_t tag;
   bool ret = false;

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (mac_key);
   BSON_ASSERT_PARAM (associated_data);
   BSON_ASSERT_PARAM (ciphertext);
   BSON_ASSERT_PARAM (out);

   _mongocrypt_buffer_init (&to_hmac);

   if (MONGOCRYPT_MAC_KEY_LEN != mac_key->len) {
      CLIENT_ERR ("HMAC key wrong length: %d", mac_key->len);
      goto done;
   }

   if (out->len != MONGOCRYPT_HMAC_LEN) {
      CLIENT_ERR ("out wrong length: %d", out->len);
      goto done;
   }

   /* [MCGREW]:
    * """
    * 4.  The octet string AL is equal to the number of bits in A expressed as a
    * 64-bit unsigned integer in network byte order.
    * 5.  A message authentication tag T is computed by applying HMAC [RFC2104]
    * to the following data, in order:
    *      the associated data A,
    *      the ciphertext S computed in the previous step, and
    *      the octet string AL defined above.
    * """
    */

   /* Add associated data. */
   _mongocrypt_buffer_init (&intermediates[0]);
   _mongocrypt_buffer_init (&intermediates[1]);
   _mongocrypt_buffer_init (&intermediates[2]);
   intermediates[0].data = associated_data->data;
   intermediates[0].len = associated_data->len;
   /* Add ciphertext. */
   intermediates[1].data = ciphertext->data;
   intermediates[1].len = ciphertext->len;
   /* Add associated data length in bits. */
   /* multiplying a uint32_t by 8 won't bring it anywhere close to UINT64_MAX */
   associated_data_len_be = 8 * (uint64_t) associated_data->len;
   associated_data_len_be = BSON_UINT64_TO_BE (associated_data_len_be);
   intermediates[2].data = (uint8_t *) &associated_data_len_be;
   intermediates[2].len = sizeof (uint64_t);
   tag.data = tag_storage;
   tag.len = sizeof (tag_storage);


   if (!_mongocrypt_buffer_concat (&to_hmac, intermediates, 3)) {
      CLIENT_ERR ("failed to allocate buffer");
      goto done;
   }
   if (!_crypto_hmac_sha_512 (crypto, mac_key, &to_hmac, &tag, status)) {
      goto done;
   }

   /* [MCGREW 2.7] "The HMAC-SHA-512 value is truncated to T_LEN=32 octets" */
   memcpy (out->data, tag.data, MONGOCRYPT_HMAC_LEN);
   ret = true;
done:
   _mongocrypt_buffer_cleanup (&to_hmac);
   return ret;
}

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_do_encryption --
 *
 *    Defer encryption to whichever crypto library libmongocrypt is using.
 *
 * Parameters:
 *    @iv a 16 byte IV.
 *    @associated_data associated data for the HMAC. May be NULL.
 *    @key a 96 byte key.
 *    @plaintext the plaintext to encrypt.
 *    @ciphertext a location for the resulting ciphertext and HMAC tag.
 *    @bytes_written a location for the resulting bytes written.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. ciphertext->data has been pre-allocated with enough space for the
 *    resulting ciphertext. Use _mongocrypt_calculate_ciphertext_len.
 *
 * Postconditions:
 *    1. bytes_written is set to the length of the written ciphertext. This
 *    is the same as
 *    _mongocrypt_calculate_ciphertext_len (plaintext->len, status).
 *
 * ----------------------------------------------------------------------------
 */
bool
_mongocrypt_do_encryption (_mongocrypt_crypto_t *crypto,
                           const _mongocrypt_buffer_t *iv,
                           const _mongocrypt_buffer_t *associated_data,
                           const _mongocrypt_buffer_t *key,
                           const _mongocrypt_buffer_t *plaintext,
                           _mongocrypt_buffer_t *ciphertext,
                           uint32_t *bytes_written,
                           mongocrypt_status_t *status)
{
   _mongocrypt_buffer_t mac_key = {0}, enc_key = {0}, intermediate = {0},
                        intermediate_hmac = {0}, empty_buffer = {0};
   uint32_t intermediate_bytes_written = 0;

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (iv);
   /* associated_data is checked at the point it is used, so it can be NULL */
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (ciphertext);

   memset (ciphertext->data, 0, ciphertext->len);

   if (ciphertext->len !=
       _mongocrypt_calculate_ciphertext_len (plaintext->len, status)) {
      CLIENT_ERR (
         "output ciphertext should have been allocated with %d bytes",
         _mongocrypt_calculate_ciphertext_len (plaintext->len, status));
      return false;
   }

   BSON_ASSERT_PARAM (bytes_written);
   *bytes_written = 0;

   if (MONGOCRYPT_IV_LEN != iv->len) {
      CLIENT_ERR ("IV should have length %d, but has length %d",
                  MONGOCRYPT_IV_LEN,
                  iv->len);
      return false;
   }
   if (MONGOCRYPT_KEY_LEN != key->len) {
      CLIENT_ERR ("key should have length %d, but has length %d",
                  MONGOCRYPT_KEY_LEN,
                  key->len);
      return false;
   }

   intermediate.len = ciphertext->len;
   intermediate.data = ciphertext->data;

   /* [MCGREW]: Step 1. "MAC_KEY consists of the initial MAC_KEY_LEN octets of
    * K, in order. ENC_KEY consists of the final ENC_KEY_LEN octets of K, in
    * order." */
   mac_key.data = (uint8_t *) key->data;
   mac_key.len = MONGOCRYPT_MAC_KEY_LEN;
   enc_key.data = (uint8_t *) key->data + MONGOCRYPT_MAC_KEY_LEN;
   enc_key.len = MONGOCRYPT_ENC_KEY_LEN;

   /* Prepend the IV. */
   memcpy (intermediate.data, iv->data, iv->len);
   intermediate.data += iv->len;
   BSON_ASSERT (intermediate.len >= iv->len);
   intermediate.len -= iv->len;
   BSON_ASSERT (*bytes_written <= UINT32_MAX - iv->len);
   *bytes_written += iv->len;

   /* [MCGREW]: Steps 2 & 3. */
   if (!_encrypt_step (crypto,
                       iv,
                       &enc_key,
                       plaintext,
                       &intermediate,
                       &intermediate_bytes_written,
                       status)) {
      return false;
   }

   BSON_ASSERT (*bytes_written <= UINT32_MAX - intermediate_bytes_written);
   *bytes_written += intermediate_bytes_written;

   /* Append the HMAC tag. */
   intermediate_hmac.data = ciphertext->data + *bytes_written;
   intermediate_hmac.len = MONGOCRYPT_HMAC_LEN;

   intermediate.data = ciphertext->data;
   intermediate.len = *bytes_written;

   /* [MCGREW]: Steps 4 & 5, compute the HMAC. */
   if (!_hmac_step (crypto,
                    &mac_key,
                    associated_data ? associated_data : &empty_buffer,
                    &intermediate,
                    &intermediate_hmac,
                    status)) {
      return false;
   }

   *bytes_written += MONGOCRYPT_HMAC_LEN;
   return true;
}


/* ----------------------------------------------------------------------------
 *
 * _aes256_cbc_decrypt --
 *
 *    Decrypts using AES256 CBC using a secret key and a known IV.
 *
 * Parameters:
 *    @enc_key a 32 byte key.
 *    @ciphertext the ciphertext to decrypt.
 *    @plaintext the resulting plaintext.
 *    @bytes_written a location for the resulting number of bytes written into
 *    plaintext->data.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. plaintext->data has been pre-allocated with enough space for the
 *    resulting plaintext.
 *
 * Postconditions:
 *    1. bytes_written is set to the length of the written plaintext, excluding
 *    padding. This may be less than
 *    _mongocrypt_calculate_plaintext_len (ciphertext->len, status).
 *
 * ----------------------------------------------------------------------------
 */
static bool
_decrypt_step (_mongocrypt_crypto_t *crypto,
               const _mongocrypt_buffer_t *iv,
               const _mongocrypt_buffer_t *enc_key,
               const _mongocrypt_buffer_t *ciphertext,
               _mongocrypt_buffer_t *plaintext,
               uint32_t *bytes_written,
               mongocrypt_status_t *status)
{
   uint8_t padding_byte;

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (iv);
   BSON_ASSERT_PARAM (enc_key);
   BSON_ASSERT_PARAM (ciphertext);
   BSON_ASSERT_PARAM (plaintext);

   BSON_ASSERT_PARAM (bytes_written);
   *bytes_written = 0;

   if (MONGOCRYPT_IV_LEN != iv->len) {
      CLIENT_ERR ("IV should have length %d, but has length %d",
                  MONGOCRYPT_IV_LEN,
                  iv->len);
      return false;
   }
   if (MONGOCRYPT_ENC_KEY_LEN != enc_key->len) {
      CLIENT_ERR ("encryption key should have length %d, but has length %d",
                  MONGOCRYPT_ENC_KEY_LEN,
                  enc_key->len);
      return false;
   }


   if (ciphertext->len % MONGOCRYPT_BLOCK_SIZE > 0) {
      CLIENT_ERR ("error, ciphertext length is not a multiple of block size");
      return false;
   }

   if (!_crypto_aes_256_cbc_decrypt (
          crypto,
          (aes_256_args_t){.iv = iv,
                           .key = enc_key,
                           .in = ciphertext,
                           .out = plaintext,
                           .bytes_written = bytes_written,
                           .status = status})) {
      return false;
   }

   BSON_ASSERT (*bytes_written > 0);
   padding_byte = plaintext->data[*bytes_written - 1];
   if (padding_byte > 16) {
      CLIENT_ERR ("error, ciphertext malformed padding");
      return false;
   }
   *bytes_written -= padding_byte;
   return true;
}


/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_do_decryption --
 *
 *    Defer decryption to whichever crypto library libmongocrypt is using.
 *
 * Parameters:
 *    @associated_data associated data for the HMAC. May be NULL.
 *    @key a 96 byte key.
 *    @ciphertext the ciphertext to decrypt. This contains the IV prepended.
 *    @plaintext a location for the resulting plaintext.
 *    @bytes_written a location for the resulting bytes written.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 *  Preconditions:
 *    1. plaintext->data has been pre-allocated with enough space for the
 *    resulting plaintext and padding. See _mongocrypt_calculate_plaintext_len.
 *
 *  Postconditions:
 *    1. bytes_written is set to the length of the written plaintext, excluding
 *    padding. This may be less than
 *    _mongocrypt_calculate_plaintext_len (ciphertext->len, status).
 *
 * ----------------------------------------------------------------------------
 */
bool
_mongocrypt_do_decryption (_mongocrypt_crypto_t *crypto,
                           const _mongocrypt_buffer_t *associated_data,
                           const _mongocrypt_buffer_t *key,
                           const _mongocrypt_buffer_t *ciphertext,
                           _mongocrypt_buffer_t *plaintext,
                           uint32_t *bytes_written,
                           mongocrypt_status_t *status)
{
   bool ret = false;
   _mongocrypt_buffer_t mac_key = {0}, enc_key = {0}, intermediate = {0},
                        hmac_tag = {0}, iv = {0}, empty_buffer = {0};
   uint8_t hmac_tag_storage[MONGOCRYPT_HMAC_LEN];

   BSON_ASSERT_PARAM (crypto);
   /* associated_data is checked at the point it is used, so it can be NULL */
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (ciphertext);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (bytes_written);

   if (plaintext->len !=
       _mongocrypt_calculate_plaintext_len (ciphertext->len, status)) {
      CLIENT_ERR ("output plaintext should have been allocated with %d bytes, "
                  "but has: %d",
                  _mongocrypt_calculate_plaintext_len (ciphertext->len, status),
                  plaintext->len);
      return false;
   }

   if (MONGOCRYPT_KEY_LEN != key->len) {
      CLIENT_ERR ("key should have length %d, but has length %d",
                  MONGOCRYPT_KEY_LEN,
                  key->len);
      return false;
   }

   if (ciphertext->len <
       MONGOCRYPT_HMAC_LEN + MONGOCRYPT_IV_LEN + MONGOCRYPT_BLOCK_SIZE) {
      CLIENT_ERR ("corrupt ciphertext - must be > %d bytes",
                  MONGOCRYPT_HMAC_LEN + MONGOCRYPT_IV_LEN +
                     MONGOCRYPT_BLOCK_SIZE);
      goto done;
   }

   mac_key.data = (uint8_t *) key->data;
   mac_key.len = MONGOCRYPT_MAC_KEY_LEN;
   enc_key.data = (uint8_t *) key->data + MONGOCRYPT_MAC_KEY_LEN;
   enc_key.len = MONGOCRYPT_ENC_KEY_LEN;

   iv.data = ciphertext->data;
   iv.len = MONGOCRYPT_IV_LEN;

   intermediate.data = (uint8_t *) ciphertext->data;
   intermediate.len = ciphertext->len - MONGOCRYPT_HMAC_LEN;

   hmac_tag.data = hmac_tag_storage;
   hmac_tag.len = MONGOCRYPT_HMAC_LEN;

   /* [MCGREW 2.2]: Step 3: HMAC check. */
   if (!_hmac_step (crypto,
                    &mac_key,
                    associated_data ? associated_data : &empty_buffer,
                    &intermediate,
                    &hmac_tag,
                    status)) {
      goto done;
   }

   /* [MCGREW] "using a comparison routine that takes constant time". */
   if (0 != _mongocrypt_memequal (hmac_tag.data,
                                  ciphertext->data +
                                     (ciphertext->len - MONGOCRYPT_HMAC_LEN),
                                  MONGOCRYPT_HMAC_LEN)) {
      CLIENT_ERR ("HMAC validation failure");
      goto done;
   }

   /* Decrypt data excluding IV + HMAC. */
   intermediate.data = (uint8_t *) ciphertext->data + MONGOCRYPT_IV_LEN;
   intermediate.len =
      ciphertext->len - (MONGOCRYPT_IV_LEN + MONGOCRYPT_HMAC_LEN);

   if (!_decrypt_step (crypto,
                       &iv,
                       &enc_key,
                       &intermediate,
                       plaintext,
                       bytes_written,
                       status)) {
      goto done;
   }

   ret = true;
done:
   return ret;
}


/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_random --
 *
 *    Generates a string of random bytes.
 *
 * Parameters:
 *    @out an output buffer that has been pre-allocated.
 *    @status set on error.
 *    @count the size of the random string in bytes.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 *  Preconditions:
 *    1. out has been pre-allocated with at least 'count' bytes of space.
 *
 * ----------------------------------------------------------------------------
 */
bool
_mongocrypt_random (_mongocrypt_crypto_t *crypto,
                    _mongocrypt_buffer_t *out,
                    uint32_t count,
                    mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (out);

   if (count != out->len) {
      CLIENT_ERR (
         "out should have length %d, but has length %d", count, out->len);
      return false;
   }

   return _crypto_random (crypto, out, count, status);
}


/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_calculate_deterministic_iv --
 *
 *    Compute the IV for deterministic encryption from the plaintext and IV
 *    key by using HMAC function.
 *
 * Parameters:
 *    @key the 96 byte key. The last 32 represent the IV key.
 *    @plaintext the plaintext to be encrypted.
 *    @associated_data associated data to include in the HMAC.
 *    @out an output buffer that has been pre-allocated.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 *  Preconditions:
 *    1. out has been pre-allocated with at least MONGOCRYPT_IV_LEN bytes.
 *
 * ----------------------------------------------------------------------------
 */
bool
_mongocrypt_calculate_deterministic_iv (
   _mongocrypt_crypto_t *crypto,
   const _mongocrypt_buffer_t *key,
   const _mongocrypt_buffer_t *plaintext,
   const _mongocrypt_buffer_t *associated_data,
   _mongocrypt_buffer_t *out,
   mongocrypt_status_t *status)
{
   _mongocrypt_buffer_t intermediates[3];
   _mongocrypt_buffer_t to_hmac;
   _mongocrypt_buffer_t iv_key;
   uint64_t associated_data_len_be;
   uint8_t tag_storage[64];
   _mongocrypt_buffer_t tag;
   bool ret = false;

   _mongocrypt_buffer_init (&to_hmac);

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (associated_data);
   BSON_ASSERT_PARAM (out);

   if (MONGOCRYPT_KEY_LEN != key->len) {
      CLIENT_ERR ("key should have length %d, but has length %d\n",
                  MONGOCRYPT_KEY_LEN,
                  key->len);
      goto done;
   }
   if (MONGOCRYPT_IV_LEN != out->len) {
      CLIENT_ERR ("out should have length %d, but has length %d\n",
                  MONGOCRYPT_IV_LEN,
                  out->len);
      goto done;
   }

   _mongocrypt_buffer_init (&iv_key);
   iv_key.data = key->data + MONGOCRYPT_ENC_KEY_LEN + MONGOCRYPT_MAC_KEY_LEN;
   iv_key.len = MONGOCRYPT_IV_KEY_LEN;

   _mongocrypt_buffer_init (&intermediates[0]);
   _mongocrypt_buffer_init (&intermediates[1]);
   _mongocrypt_buffer_init (&intermediates[2]);
   /* Add associated data. */
   intermediates[0].data = associated_data->data;
   intermediates[0].len = associated_data->len;
   /* Add associated data length in bits. */
   /* multiplying a uint32_t by 8 won't bring it anywhere close to UINT64_MAX */
   associated_data_len_be = 8 * (uint64_t) associated_data->len;
   associated_data_len_be = BSON_UINT64_TO_BE (associated_data_len_be);
   intermediates[1].data = (uint8_t *) &associated_data_len_be;
   intermediates[1].len = sizeof (uint64_t);
   /* Add plaintext. */
   intermediates[2].data = (uint8_t *) plaintext->data;
   intermediates[2].len = plaintext->len;

   tag.data = tag_storage;
   tag.len = sizeof (tag_storage);

   if (!_mongocrypt_buffer_concat (&to_hmac, intermediates, 3)) {
      CLIENT_ERR ("failed to allocate buffer");
      goto done;
   }

   if (!_crypto_hmac_sha_512 (crypto, &iv_key, &to_hmac, &tag, status)) {
      goto done;
   }

   /* Truncate to IV length */
   memcpy (out->data, tag.data, MONGOCRYPT_IV_LEN);

   ret = true;
done:
   _mongocrypt_buffer_cleanup (&to_hmac);
   return ret;
}

bool
_mongocrypt_wrap_key (_mongocrypt_crypto_t *crypto,
                      _mongocrypt_buffer_t *kek,
                      _mongocrypt_buffer_t *dek,
                      _mongocrypt_buffer_t *encrypted_dek,
                      mongocrypt_status_t *status)
{
   uint32_t bytes_written;
   _mongocrypt_buffer_t iv = {0};
   bool ret = false;

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (kek);
   BSON_ASSERT_PARAM (dek);
   BSON_ASSERT_PARAM (encrypted_dek);

   _mongocrypt_buffer_init (encrypted_dek);

   if (dek->len != MONGOCRYPT_KEY_LEN) {
      CLIENT_ERR ("data encryption key is incorrect length, expected: %" PRIu32
                  ", got: %" PRIu32,
                  MONGOCRYPT_KEY_LEN,
                  dek->len);
      goto done;
   }

   _mongocrypt_buffer_resize (
      encrypted_dek, _mongocrypt_calculate_ciphertext_len (dek->len, status));
   _mongocrypt_buffer_resize (&iv, MONGOCRYPT_IV_LEN);

   if (!_mongocrypt_random (crypto, &iv, MONGOCRYPT_IV_LEN, status)) {
      goto done;
   }

   if (!_mongocrypt_do_encryption (crypto,
                                   &iv,
                                   NULL /* associated data. */,
                                   kek,
                                   dek,
                                   encrypted_dek,
                                   &bytes_written,
                                   status)) {
      goto done;
   }

   ret = true;
done:
   _mongocrypt_buffer_cleanup (&iv);
   return ret;
}

bool
_mongocrypt_unwrap_key (_mongocrypt_crypto_t *crypto,
                        _mongocrypt_buffer_t *kek,
                        _mongocrypt_buffer_t *encrypted_dek,
                        _mongocrypt_buffer_t *dek,
                        mongocrypt_status_t *status)
{
   uint32_t bytes_written;

   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (kek);
   BSON_ASSERT_PARAM (dek);
   BSON_ASSERT_PARAM (encrypted_dek);

   _mongocrypt_buffer_init (dek);
   _mongocrypt_buffer_resize (
      dek, _mongocrypt_calculate_plaintext_len (encrypted_dek->len, status));

   if (!_mongocrypt_do_decryption (crypto,
                                   NULL /* associated data. */,
                                   kek,
                                   encrypted_dek,
                                   dek,
                                   &bytes_written,
                                   status)) {
      return false;
   }
   dek->len = bytes_written;

   if (dek->len != MONGOCRYPT_KEY_LEN) {
      CLIENT_ERR ("decrypted key is incorrect length, expected: %" PRIu32
                  ", got: %" PRIu32,
                  MONGOCRYPT_KEY_LEN,
                  dek->len);
      return false;
   }
   return true;
}

bool
_mongocrypt_hmac_sha_256 (_mongocrypt_crypto_t *crypto,
                          const _mongocrypt_buffer_t *key,
                          const _mongocrypt_buffer_t *in,
                          _mongocrypt_buffer_t *out,
                          mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (in);
   BSON_ASSERT_PARAM (out);

   if (key->len != MONGOCRYPT_MAC_KEY_LEN) {
      CLIENT_ERR ("invalid hmac_sha_256 key length. Got %" PRIu32
                  ", expected: %" PRIu32,
                  key->len,
                  MONGOCRYPT_MAC_KEY_LEN);
      return false;
   }

   if (crypto->hooks_enabled) {
      mongocrypt_binary_t key_bin, out_bin, in_bin;
      _mongocrypt_buffer_to_binary (key, &key_bin);
      _mongocrypt_buffer_to_binary (out, &out_bin);
      _mongocrypt_buffer_to_binary (in, &in_bin);

      return crypto->hmac_sha_256 (
         crypto->ctx, &key_bin, &in_bin, &out_bin, status);
   }
   return _native_crypto_hmac_sha_256 (key, in, out, status);
}

bool
_mongocrypt_fle2aead_do_encryption (_mongocrypt_crypto_t *crypto,
                                    const _mongocrypt_buffer_t *iv,
                                    const _mongocrypt_buffer_t *associated_data,
                                    const _mongocrypt_buffer_t *key,
                                    const _mongocrypt_buffer_t *plaintext,
                                    _mongocrypt_buffer_t *ciphertext,
                                    uint32_t *bytes_written,
                                    mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (iv);
   BSON_ASSERT_PARAM (associated_data);
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (ciphertext);
   BSON_ASSERT_PARAM (bytes_written);

   if (ciphertext->len !=
       _mongocrypt_fle2aead_calculate_ciphertext_len (plaintext->len, status)) {
      CLIENT_ERR ("output ciphertext must be allocated with %" PRIu32 " bytes",
                  _mongocrypt_fle2aead_calculate_ciphertext_len (plaintext->len,
                                                                 status));
      return false;
   }

   if (plaintext->len <= 0) {
      CLIENT_ERR ("input plaintext too small. Must be more than zero bytes.");
      return false;
   }

   if (MONGOCRYPT_IV_LEN != iv->len) {
      CLIENT_ERR ("IV must be length %d, but is length %" PRIu32,
                  MONGOCRYPT_IV_LEN,
                  iv->len);
      return false;
   }
   if (MONGOCRYPT_KEY_LEN != key->len) {
      CLIENT_ERR ("key must be length %d, but is length %" PRIu32,
                  MONGOCRYPT_KEY_LEN,
                  key->len);
      return false;
   }

   memset (ciphertext->data, 0, ciphertext->len);
   *bytes_written = 0;

   /* Declare variable names matching [AEAD with
    * CTR](https://docs.google.com/document/d/1eCU7R8Kjr-mdyz6eKvhNIDVmhyYQcAaLtTfHeK7a_vE/).
    */
   /* M is the input plaintext. */
   _mongocrypt_buffer_t M;
   if (!_mongocrypt_buffer_from_subrange (&M, plaintext, 0, plaintext->len)) {
      CLIENT_ERR ("unable to create M view from plaintext");
      return false;
   }
   /* Ke is 32 byte Key for encryption. */
   _mongocrypt_buffer_t Ke;
   if (!_mongocrypt_buffer_from_subrange (
          &Ke, key, 0, MONGOCRYPT_ENC_KEY_LEN)) {
      CLIENT_ERR ("unable to create Ke view from key");
      return false;
   }
   /* IV is 16 byte IV. */
   _mongocrypt_buffer_t IV;
   if (!_mongocrypt_buffer_from_subrange (&IV, iv, 0, iv->len)) {
      CLIENT_ERR ("unable to create IV view from iv");
      return false;
   }
   /* Km is 32 byte Key for HMAC. */
   _mongocrypt_buffer_t Km;
   if (!_mongocrypt_buffer_from_subrange (
          &Km, key, MONGOCRYPT_ENC_KEY_LEN, MONGOCRYPT_MAC_KEY_LEN)) {
      CLIENT_ERR ("unable to create Km view from key");
      return false;
   }
   /* AD is Associated Data. */
   _mongocrypt_buffer_t AD;
   if (!_mongocrypt_buffer_from_subrange (
          &AD, associated_data, 0, associated_data->len)) {
      CLIENT_ERR ("unable to create AD view from associated_data");
      return false;
   }
   /* C is the output ciphertext. */
   _mongocrypt_buffer_t C;
   if (!_mongocrypt_buffer_from_subrange (&C, ciphertext, 0, ciphertext->len)) {
      CLIENT_ERR ("unable to create C view from ciphertext");
      return false;
   }
   /* S is the output of the symmetric cipher. It is appended after IV in C. */
   _mongocrypt_buffer_t S;
   BSON_ASSERT (C.len >= MONGOCRYPT_IV_LEN + MONGOCRYPT_HMAC_LEN);
   if (!_mongocrypt_buffer_from_subrange (&S,
                                          &C,
                                          MONGOCRYPT_IV_LEN,
                                          C.len - MONGOCRYPT_IV_LEN -
                                             MONGOCRYPT_HMAC_LEN)) {
      CLIENT_ERR ("unable to create S view from C");
      return false;
   }
   uint32_t S_bytes_written = 0;
   /* T is the output of the HMAC tag. It is appended after S in C. */
   _mongocrypt_buffer_t T;
   if (!_mongocrypt_buffer_from_subrange (
          &T, &C, C.len - MONGOCRYPT_HMAC_LEN, MONGOCRYPT_HMAC_LEN)) {
      CLIENT_ERR ("unable to create T view from C");
      return false;
   }

   /* Compute S = AES-CTR.Enc(Ke, IV, M). */
   if (!_crypto_aes_256_ctr_encrypt (
          crypto,
          (aes_256_args_t){.key = &Ke,
                           .iv = &IV,
                           .in = &M,
                           .out = &S,
                           .bytes_written = &S_bytes_written,
                           .status = status})) {
      return false;
   }

   /* Compute T = HMAC-SHA256(Km, AD || IV || S). */
   {
      _mongocrypt_buffer_t hmac_inputs[] = {AD, IV, S};
      _mongocrypt_buffer_t hmac_input = {0};
      _mongocrypt_buffer_concat (&hmac_input, hmac_inputs, 3);
      if (!_mongocrypt_hmac_sha_256 (crypto, &Km, &hmac_input, &T, status)) {
         _mongocrypt_buffer_cleanup (&hmac_input);
         return false;
      }
      _mongocrypt_buffer_cleanup (&hmac_input);
   }

   /* Output C = IV || S || T. */
   /* S and T are already in C. Prepend IV. */
   memmove (C.data, IV.data, MONGOCRYPT_IV_LEN);

   *bytes_written = MONGOCRYPT_IV_LEN + S_bytes_written + MONGOCRYPT_HMAC_LEN;
   return true;
}

bool
_mongocrypt_fle2aead_do_decryption (_mongocrypt_crypto_t *crypto,
                                    const _mongocrypt_buffer_t *associated_data,
                                    const _mongocrypt_buffer_t *key,
                                    const _mongocrypt_buffer_t *ciphertext,
                                    _mongocrypt_buffer_t *plaintext,
                                    uint32_t *bytes_written,
                                    mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (associated_data);
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (ciphertext);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (bytes_written);

   if (ciphertext->len <= MONGOCRYPT_IV_LEN + MONGOCRYPT_HMAC_LEN) {
      CLIENT_ERR ("input ciphertext too small. Must be more than %" PRIu32
                  " bytes",
                  MONGOCRYPT_IV_LEN + MONGOCRYPT_HMAC_LEN);
      return false;
   }

   if (plaintext->len !=
       _mongocrypt_fle2aead_calculate_plaintext_len (ciphertext->len, status)) {
      CLIENT_ERR ("output plaintext must be allocated with %" PRIu32 " bytes",
                  _mongocrypt_fle2aead_calculate_plaintext_len (ciphertext->len,
                                                                status));
      return false;
   }

   if (MONGOCRYPT_KEY_LEN != key->len) {
      CLIENT_ERR ("key must be length %d, but is length %" PRIu32,
                  MONGOCRYPT_KEY_LEN,
                  key->len);
      return false;
   }

   memset (plaintext->data, 0, plaintext->len);
   *bytes_written = 0;

   /* Declare variable names matching [AEAD with
    * CTR](https://docs.google.com/document/d/1eCU7R8Kjr-mdyz6eKvhNIDVmhyYQcAaLtTfHeK7a_vE/).
    */
   /* C is the input ciphertext. */
   _mongocrypt_buffer_t C;
   if (!_mongocrypt_buffer_from_subrange (&C, ciphertext, 0, ciphertext->len)) {
      CLIENT_ERR ("unable to create C view from ciphertext");
      return false;
   }
   /* IV is 16 byte IV. It is the first part of C. */
   _mongocrypt_buffer_t IV;
   if (!_mongocrypt_buffer_from_subrange (
          &IV, ciphertext, 0, MONGOCRYPT_IV_LEN)) {
      CLIENT_ERR ("unable to create IV view from ciphertext");
      return false;
   }
   /* S is the symmetric cipher output from C. It is after the IV in C. */
   _mongocrypt_buffer_t S;
   if (!_mongocrypt_buffer_from_subrange (&S,
                                          ciphertext,
                                          MONGOCRYPT_IV_LEN,
                                          C.len - MONGOCRYPT_IV_LEN -
                                             MONGOCRYPT_HMAC_LEN)) {
      CLIENT_ERR ("unable to create S view from C");
      return false;
   }
   /* T is the HMAC tag from C. It is after S in C. */
   _mongocrypt_buffer_t T;
   if (!_mongocrypt_buffer_from_subrange (
          &T, &C, C.len - MONGOCRYPT_HMAC_LEN, MONGOCRYPT_HMAC_LEN)) {
      CLIENT_ERR ("unable to create T view from C");
      return false;
   }
   /* Tp is the computed HMAC of the input. */
   _mongocrypt_buffer_t Tp = {0};
   /* M is the output plaintext. */
   _mongocrypt_buffer_t M;
   if (!_mongocrypt_buffer_from_subrange (&M, plaintext, 0, plaintext->len)) {
      CLIENT_ERR ("unable to create M view from plaintext");
      return false;
   }
   /* Ke is 32 byte Key for encryption. */
   _mongocrypt_buffer_t Ke;
   if (!_mongocrypt_buffer_from_subrange (
          &Ke, key, 0, MONGOCRYPT_ENC_KEY_LEN)) {
      CLIENT_ERR ("unable to create Ke view from key");
      return false;
   }
   /* Km is 32 byte Key for HMAC. */
   _mongocrypt_buffer_t Km;
   if (!_mongocrypt_buffer_from_subrange (
          &Km, key, MONGOCRYPT_ENC_KEY_LEN, MONGOCRYPT_MAC_KEY_LEN)) {
      CLIENT_ERR ("unable to create Km view from key");
      return false;
   }
   /* AD is Associated Data. */
   _mongocrypt_buffer_t AD;
   if (!_mongocrypt_buffer_from_subrange (
          &AD, associated_data, 0, associated_data->len)) {
      CLIENT_ERR ("unable to create AD view from associated_data");
      return false;
   }

   /* Compute Tp = HMAC-SHA256(Km, AD || IV || S). Check that it matches input
    * ciphertext T. */
   {
      _mongocrypt_buffer_t hmac_inputs[] = {AD, IV, S};
      _mongocrypt_buffer_t hmac_input = {0};
      _mongocrypt_buffer_concat (&hmac_input, hmac_inputs, 3);
      _mongocrypt_buffer_resize (&Tp, MONGOCRYPT_HMAC_LEN);
      if (!_mongocrypt_hmac_sha_256 (crypto, &Km, &hmac_input, &Tp, status)) {
         _mongocrypt_buffer_cleanup (&hmac_input);
         _mongocrypt_buffer_cleanup (&Tp);
         return false;
      }
      if (0 != _mongocrypt_buffer_cmp (&T, &Tp)) {
         CLIENT_ERR ("decryption error");
         _mongocrypt_buffer_cleanup (&hmac_input);
         _mongocrypt_buffer_cleanup (&Tp);
         return false;
      }
      _mongocrypt_buffer_cleanup (&hmac_input);
      _mongocrypt_buffer_cleanup (&Tp);
   }

   /* Compute and output M = AES-CTR.Dec(Ke, S) */
   if (!_crypto_aes_256_ctr_decrypt (
          crypto,
          (aes_256_args_t){.key = &Ke,
                           .iv = &IV,
                           .in = &S,
                           .out = &M,
                           .bytes_written = bytes_written,
                           .status = status})) {
      return false;
   }

   return true;
}

bool
_mongocrypt_fle2_do_encryption (_mongocrypt_crypto_t *crypto,
                                const _mongocrypt_buffer_t *iv,
                                const _mongocrypt_buffer_t *key,
                                const _mongocrypt_buffer_t *plaintext,
                                _mongocrypt_buffer_t *ciphertext,
                                uint32_t *bytes_written,
                                mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (iv);
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (ciphertext);
   BSON_ASSERT_PARAM (bytes_written);

   if (ciphertext->len !=
       _mongocrypt_fle2_calculate_ciphertext_len (plaintext->len, status)) {
      CLIENT_ERR (
         "output ciphertext must be allocated with %" PRIu32 " bytes",
         _mongocrypt_fle2_calculate_ciphertext_len (plaintext->len, status));
      return false;
   }

   if (plaintext->len <= 0) {
      CLIENT_ERR ("input plaintext too small. Must be more than zero bytes.");
      return false;
   }

   if (MONGOCRYPT_IV_LEN != iv->len) {
      CLIENT_ERR ("IV must be length %d, but is length %" PRIu32,
                  MONGOCRYPT_IV_LEN,
                  iv->len);
      return false;
   }
   if (MONGOCRYPT_ENC_KEY_LEN != key->len) {
      CLIENT_ERR ("key must be length %d, but is length %" PRIu32,
                  MONGOCRYPT_ENC_KEY_LEN,
                  key->len);
      return false;
   }

   BSON_ASSERT (ciphertext->len >= MONGOCRYPT_IV_LEN);
   memset (ciphertext->data + MONGOCRYPT_IV_LEN,
           0,
           ciphertext->len - MONGOCRYPT_IV_LEN);
   *bytes_written = 0;

   /* Declare variable names matching [AEAD with
    * CTR](https://docs.google.com/document/d/1eCU7R8Kjr-mdyz6eKvhNIDVmhyYQcAaLtTfHeK7a_vE/).
    */
   /* M is the input plaintext. */
   _mongocrypt_buffer_t M = *plaintext;
   /* Ke is 32 byte Key for encryption. */
   _mongocrypt_buffer_t Ke = *key;
   /* IV is 16 byte IV. */
   _mongocrypt_buffer_t IV = *iv;
   /* C is the output ciphertext. */
   _mongocrypt_buffer_t C = *ciphertext;
   /* S is the output of the symmetric cipher. It is appended after IV in C. */
   _mongocrypt_buffer_t S;
   if (!_mongocrypt_buffer_from_subrange (
          &S, &C, MONGOCRYPT_IV_LEN, C.len - MONGOCRYPT_IV_LEN)) {
      CLIENT_ERR ("unable to create S view from C");
      return false;
   }
   uint32_t S_bytes_written = 0;

   /* Compute S = AES-CTR.Enc(Ke, IV, M). */
   if (!_crypto_aes_256_ctr_encrypt (
          crypto,
          (aes_256_args_t){.key = &Ke,
                           .iv = &IV,
                           .in = &M,
                           .out = &S,
                           .bytes_written = &S_bytes_written,
                           .status = status})) {
      return false;
   }

   if (S_bytes_written != M.len) {
      CLIENT_ERR ("expected S_bytes_written=%" PRIu32 " got %" PRIu32,
                  M.len,
                  S_bytes_written);
      return false;
   }

   /* Output C = IV || S. */
   /* S is already in C. Prepend IV. */
   memmove (C.data, IV.data, MONGOCRYPT_IV_LEN);

   *bytes_written = MONGOCRYPT_IV_LEN + S_bytes_written;
   return true;
}

bool
_mongocrypt_fle2_do_decryption (_mongocrypt_crypto_t *crypto,
                                const _mongocrypt_buffer_t *key,
                                const _mongocrypt_buffer_t *ciphertext,
                                _mongocrypt_buffer_t *plaintext,
                                uint32_t *bytes_written,
                                mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (key);
   BSON_ASSERT_PARAM (ciphertext);
   BSON_ASSERT_PARAM (plaintext);
   BSON_ASSERT_PARAM (bytes_written);

   if (ciphertext->len <= MONGOCRYPT_IV_LEN) {
      CLIENT_ERR ("input ciphertext too small. Must be more than %" PRIu32
                  " bytes",
                  MONGOCRYPT_IV_LEN);
      return false;
   }

   if (plaintext->len !=
       _mongocrypt_fle2_calculate_plaintext_len (ciphertext->len, status)) {
      CLIENT_ERR (
         "output plaintext must be allocated with %" PRIu32 " bytes",
         _mongocrypt_fle2_calculate_plaintext_len (ciphertext->len, status));
      return false;
   }

   if (MONGOCRYPT_ENC_KEY_LEN != key->len) {
      CLIENT_ERR ("key must be length %d, but is length %" PRIu32,
                  MONGOCRYPT_ENC_KEY_LEN,
                  key->len);
      return false;
   }

   memset (plaintext->data, 0, plaintext->len);
   *bytes_written = 0;

   /* Declare variable names matching [AEAD with
    * CTR](https://docs.google.com/document/d/1eCU7R8Kjr-mdyz6eKvhNIDVmhyYQcAaLtTfHeK7a_vE/).
    */
   /* C is the input ciphertext. */
   _mongocrypt_buffer_t C = *ciphertext;
   /* IV is 16 byte IV. It is the first part of C. */
   _mongocrypt_buffer_t IV;
   if (!_mongocrypt_buffer_from_subrange (
          &IV, ciphertext, 0, MONGOCRYPT_IV_LEN)) {
      CLIENT_ERR ("unable to create IV view from ciphertext");
      return false;
   }
   /* S is the symmetric cipher output from C. It is after the IV in C. */
   _mongocrypt_buffer_t S;
   if (!_mongocrypt_buffer_from_subrange (
          &S, ciphertext, MONGOCRYPT_IV_LEN, C.len - MONGOCRYPT_IV_LEN)) {
      CLIENT_ERR ("unable to create S view from C");
      return false;
   }
   /* M is the output plaintext. */
   _mongocrypt_buffer_t M = *plaintext;
   /* Ke is 32 byte Key for encryption. */
   _mongocrypt_buffer_t Ke = *key;

   /* Compute and output M = AES-CTR.Dec(Ke, S) */
   if (!_crypto_aes_256_ctr_decrypt (
          crypto,
          (aes_256_args_t){.key = &Ke,
                           .iv = &IV,
                           .in = &S,
                           .out = &M,
                           .bytes_written = bytes_written,
                           .status = status})) {
      return false;
   }

   if (*bytes_written != S.len) {
      CLIENT_ERR ("expected bytes_written=%" PRIu32 " got %" PRIu32,
                  S.len,
                  *bytes_written);
      return false;
   }

   return true;
}

/* This implementation avoids modulo bias. It is based on arc4random_uniform:
https://github.com/openbsd/src/blob/2207c4325726fdc5c4bcd0011af0fdf7d3dab137/lib/libc/crypt/arc4random_uniform.c#L33
*/
bool
_mongocrypt_random_uint64 (_mongocrypt_crypto_t *crypto,
                           uint64_t exclusive_upper_bound,
                           uint64_t *out,
                           mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (out);

   *out = 0;

   if (exclusive_upper_bound < 2) {
      *out = 0;
      return true;
   }

   /* 2**64 % x == (2**64 - x) % x */
   uint64_t min = (0 - exclusive_upper_bound) % exclusive_upper_bound;

   _mongocrypt_buffer_t rand_u64_buf;
   _mongocrypt_buffer_init (&rand_u64_buf);
   _mongocrypt_buffer_resize (&rand_u64_buf, (uint32_t) sizeof (uint64_t));

   uint64_t rand_u64;
   for (;;) {
      if (!_mongocrypt_random (
             crypto, &rand_u64_buf, rand_u64_buf.len, status)) {
         _mongocrypt_buffer_cleanup (&rand_u64_buf);
         return false;
      }

      memcpy (&rand_u64, rand_u64_buf.data, rand_u64_buf.len);

      if (rand_u64 >= min) {
         break;
      }
   }

   *out = rand_u64 % exclusive_upper_bound;

   _mongocrypt_buffer_cleanup (&rand_u64_buf);
   return true;
}

bool
_mongocrypt_random_int64 (_mongocrypt_crypto_t *crypto,
                          int64_t exclusive_upper_bound,
                          int64_t *out,
                          mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (crypto);
   BSON_ASSERT_PARAM (out);

   if (exclusive_upper_bound <= 0) {
      CLIENT_ERR ("Expected exclusive_upper_bound > 0");
      return false;
   }

   uint64_t u64_exclusive_upper_bound = (uint64_t) exclusive_upper_bound;
   uint64_t u64_out;

   if (!_mongocrypt_random_uint64 (
          crypto, u64_exclusive_upper_bound, &u64_out, status)) {
      return false;
   }

   /* Zero the leading bit to ensure rand_i64 is non-negative. */
   u64_out &= (~(1ull << 63));
   *out = (int64_t) u64_out;
   return true;
}
