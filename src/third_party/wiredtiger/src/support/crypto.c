/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_decrypt --
 *     Common code to decrypt and verify the encrypted data in a WT_ITEM and return the decrypted
 *     buffer.
 */
int
__wt_decrypt(
  WT_SESSION_IMPL *session, WT_ENCRYPTOR *encryptor, size_t skip, WT_ITEM *in, WT_ITEM *out)
{
    size_t encryptor_data_len, result_len;
    uint32_t encrypt_len;
    uint8_t *dst, *src;

    encrypt_len = WT_STORE_SIZE(*((uint32_t *)((uint8_t *)in->data + skip)));
#ifdef WORDS_BIGENDIAN
    encrypt_len = __wt_bswap32(encrypt_len);
#endif

    if (encrypt_len > in->size)
        WT_RET_MSG(
          session, WT_ERROR, "corrupted encrypted item: padded size less than actual size");
    /*
     * We're allocating the number of bytes we're expecting from decryption plus the unencrypted
     * header.
     */
    WT_RET(__wt_buf_initsize(session, out, encrypt_len));

    src = (uint8_t *)in->data + skip + WT_ENCRYPT_LEN_SIZE;
    encryptor_data_len = encrypt_len - (skip + WT_ENCRYPT_LEN_SIZE);
    dst = (uint8_t *)out->mem + skip;

    WT_RET(encryptor->decrypt(
      encryptor, &session->iface, src, encryptor_data_len, dst, encryptor_data_len, &result_len));
    /*
     * We require encryption to be byte for byte. It should not expand the data.
     */
    WT_ASSERT(session, result_len <= encryptor_data_len);

    /*
     * Copy in the skipped header bytes.
     */
    memcpy(out->mem, in->data, skip);

    /*
     * Set the real result length in the output buffer including the skipped header size. The
     * encryptor may have done its own padding so the returned result length is the real data length
     * after decryption removes any of its padding.
     */
    out->size = result_len + skip;

    return (0);
}

/*
 * __wt_encrypt --
 *     Common code to encrypt a WT_ITEM and return the encrypted buffer.
 */
int
__wt_encrypt(
  WT_SESSION_IMPL *session, WT_KEYED_ENCRYPTOR *kencryptor, size_t skip, WT_ITEM *in, WT_ITEM *out)
{
    size_t dst_len, result_len, src_len;
    uint32_t *unpadded_lenp;
    uint8_t *dst, *src;

    /* Skip the header bytes of the source data. */
    src = (uint8_t *)in->mem + skip;
    src_len = in->size - skip;

    unpadded_lenp = (uint32_t *)((uint8_t *)out->mem + skip);

    /*
     * Skip the header bytes and the length we store in the destination data. Add in the encryptor
     * size constant to the expected destination length.
     */
    dst = (uint8_t *)out->mem + skip + WT_ENCRYPT_LEN_SIZE;
    dst_len = src_len + kencryptor->size_const;

    WT_RET(kencryptor->encryptor->encrypt(
      kencryptor->encryptor, &session->iface, src, src_len, dst, dst_len, &result_len));
    /*
     * We require encryption to be byte for byte. It should never expand the data.
     */
    WT_ASSERT(session, result_len <= dst_len);

    /*
     * The final result length includes the skipped lengths.
     */
    result_len += skip + WT_ENCRYPT_LEN_SIZE;
    /*
     * Store original size so we know how much space is needed on the decryption side.
     */
    *unpadded_lenp = WT_STORE_SIZE(result_len);
#ifdef WORDS_BIGENDIAN
    *unpadded_lenp = __wt_bswap32(*unpadded_lenp);
#endif
    /*
     * Copy in the skipped header bytes, set the final data size.
     */
    memcpy(out->mem, in->mem, skip);
    out->size = result_len;
    return (0);
}

/*
 * __wt_encrypt_size --
 *     Return the size needed for the destination buffer.
 */
void
__wt_encrypt_size(
  WT_SESSION_IMPL *session, WT_KEYED_ENCRYPTOR *kencryptor, size_t incoming_size, size_t *sizep)
{
    WT_UNUSED(session);

    if (sizep == NULL)
        return;

    *sizep = incoming_size + kencryptor->size_const + WT_ENCRYPT_LEN_SIZE;
}
