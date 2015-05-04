/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_decrypt --
 *	Common code to decrypt and verify the encrypted data in a
 *	WT_ITEM and return the decrypted buffer.
 */
int
__wt_decrypt(WT_SESSION_IMPL *session, WT_ENCRYPTOR *encryptor,
    size_t skip, WT_ITEM *in, WT_ITEM **out, size_t *result_lenp)
{
	WT_DECL_RET;
	size_t encryptor_data_len, result_len;
	uint32_t encrypt_len;
	uint8_t *dst, *src;

	encrypt_len = WT_STORE_SIZE(*((uint32_t *)
	    ((uint8_t *)in->data + skip)));
	if (encrypt_len > in->size)
		WT_ERR_MSG(session, WT_ERROR,
		    "corrupted encrypted item: padded size less than "
		    "actual size");
	/*
	 * We're allocating the exact number of bytes we're expecting
	 * from decryption plus the unencrypted header.
	 */
	WT_ERR(__wt_buf_initsize(session, *out, encrypt_len));

	src = (uint8_t *)in->data + skip + WT_ENCRYPT_LEN_SIZE;
	encryptor_data_len = encrypt_len - (skip + WT_ENCRYPT_LEN_SIZE);
	dst = (uint8_t *)(*out)->mem + skip;

	WT_ERR(encryptor->decrypt(encryptor, &session->iface,
	    src, encryptor_data_len,
	    dst, encryptor_data_len, &result_len));

	/*
	 * Copy in the skipped header bytes.  This overwrites the
	 * checksum we verified.
	 */
	memcpy((*out)->mem, in->data, skip);
	if (result_lenp != NULL)
		*result_lenp = result_len;
err:
	return (ret);
}

/*
 * __wt_encrypt --
 *	Common code to encrypt a WT_ITEM and return the encrypted buffer.
 */
int
__wt_encrypt(WT_SESSION_IMPL *session,
    WT_KEYED_ENCRYPTOR *kencryptor, size_t skip, WT_ITEM *in, WT_ITEM *out)
{
	WT_DECL_RET;
	size_t dst_len, result_len, src_len;
	uint32_t *unpadded_lenp;
	uint8_t *dst, *src;

	/* Skip the header bytes of the source data. */
	src = (uint8_t *)in->mem + skip;
	src_len = in->size - skip;

	unpadded_lenp = (uint32_t *)((uint8_t *)out->mem + skip);

	/* Skip the header bytes of the destination data. */
	dst = (uint8_t *)out->mem + skip + WT_ENCRYPT_LEN_SIZE;
	dst_len = src_len + kencryptor->size_const;

	/*
	 * Send in the temporary source address and length that
	 * includes the checksum we injected.
	 */
	WT_ERR(kencryptor->encryptor->encrypt(kencryptor->encryptor,
	    &session->iface, src, src_len, dst, dst_len, &result_len));

	/*
	 * The final result length includes the skipped lengths.
	 */
	result_len += skip + WT_ENCRYPT_LEN_SIZE;
	/*
	 * Store original size so we know how much space is needed
	 * on the decryption side.
	 */
	*unpadded_lenp = WT_STORE_SIZE(result_len);
	/*
	 * Copy in the skipped header bytes, set the final data
	 * size.
	 */
	memcpy(out->mem, in->mem, skip);
	out->size = result_len;
err:
	return (ret);
}

/*
 * __wt_encrypt_size --
 *	Return the size needed for the destination buffer.
 */
void
__wt_encrypt_size(WT_SESSION_IMPL *session, WT_KEYED_ENCRYPTOR *kencryptor,
    size_t incoming_size, size_t *sizep)
{
	WT_UNUSED(session);
	if (sizep == NULL)
		return;
	*sizep = incoming_size + kencryptor->size_const +
	    WT_ENCRYPT_LEN_SIZE;
	return;
}
