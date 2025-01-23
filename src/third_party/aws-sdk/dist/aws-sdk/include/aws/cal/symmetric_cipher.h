#ifndef AWS_CAL_SYMMETRIC_CIPHER_H
#define AWS_CAL_SYMMETRIC_CIPHER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/cal.h>
#include <aws/common/byte_buf.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_AES_256_CIPHER_BLOCK_SIZE 16
#define AWS_AES_256_KEY_BIT_LEN 256
#define AWS_AES_256_KEY_BYTE_LEN (AWS_AES_256_KEY_BIT_LEN / 8)

struct aws_symmetric_cipher;

typedef struct aws_symmetric_cipher *(aws_aes_cbc_256_new_fn)(struct aws_allocator *allocator,
                                                              const struct aws_byte_cursor *key,
                                                              const struct aws_byte_cursor *iv);

typedef struct aws_symmetric_cipher *(aws_aes_ctr_256_new_fn)(struct aws_allocator *allocator,
                                                              const struct aws_byte_cursor *key,
                                                              const struct aws_byte_cursor *iv);

typedef struct aws_symmetric_cipher *(aws_aes_gcm_256_new_fn)(struct aws_allocator *allocator,
                                                              const struct aws_byte_cursor *key,
                                                              const struct aws_byte_cursor *iv,
                                                              const struct aws_byte_cursor *aad);

typedef struct aws_symmetric_cipher *(aws_aes_keywrap_256_new_fn)(struct aws_allocator *allocator,
                                                                  const struct aws_byte_cursor *key);

enum aws_symmetric_cipher_state {
    AWS_SYMMETRIC_CIPHER_READY,
    AWS_SYMMETRIC_CIPHER_FINALIZED,
    AWS_SYMMETRIC_CIPHER_ERROR,
};

AWS_EXTERN_C_BEGIN

/**
 * Creates an instance of AES CBC with 256-bit key.
 * If key and iv are NULL, they will be generated internally.
 * You can get the generated key and iv back by calling:
 *
 * aws_symmetric_cipher_get_key() and
 * aws_symmetric_cipher_get_initialization_vector()
 *
 * respectively.
 *
 * If they are set, that key and iv will be copied internally and used by the cipher.
 *
 * Returns NULL on failure. You can check aws_last_error() to get the error code indicating the failure cause.
 */
AWS_CAL_API struct aws_symmetric_cipher *aws_aes_cbc_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv);

/**
 * Creates an instance of AES CTR with 256-bit key.
 * If key and iv are NULL, they will be generated internally.
 * You can get the generated key and iv back by calling:
 *
 * aws_symmetric_cipher_get_key() and
 * aws_symmetric_cipher_get_initialization_vector()
 *
 * respectively.
 *
 * If they are set, that key and iv will be copied internally and used by the cipher.
 *
 * Returns NULL on failure. You can check aws_last_error() to get the error code indicating the failure cause.
 */
AWS_CAL_API struct aws_symmetric_cipher *aws_aes_ctr_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv);

/**
 * Creates an instance of AES GCM with 256-bit key.
 * If key, iv are NULL, they will be generated internally.
 * You can get the generated key and iv back by calling:
 *
 * aws_symmetric_cipher_get_key() and
 * aws_symmetric_cipher_get_initialization_vector()
 *
 * respectively.
 *
 * If aad is set it will be copied and applied to the cipher.
 *
 * If they are set, that key and iv will be copied internally and used by the cipher.
 *
 * For decryption purposes tag can be provided via aws_symmetric_cipher_set_tag method.
 * Note: for decrypt operations, tag must be provided before first decrypt is called.
 * (this is a windows bcrypt limitations, but for consistency sake same limitation is extended to other platforms)
 * Tag generated during encryption can be retrieved using aws_symmetric_cipher_get_tag method
 * after finalize is called.
 *
 * Returns NULL on failure. You can check aws_last_error() to get the error code indicating the failure cause.
 */
AWS_CAL_API struct aws_symmetric_cipher *aws_aes_gcm_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv,
    const struct aws_byte_cursor *aad);

/**
 * Creates an instance of AES Keywrap with 256-bit key.
 * If key is NULL, it will be generated internally.
 * You can get the generated key back by calling:
 *
 * aws_symmetric_cipher_get_key()
 *
 * If key is set, that key will be copied internally and used by the cipher.
 *
 * Returns NULL on failure. You can check aws_last_error() to get the error code indicating the failure cause.
 */
AWS_CAL_API struct aws_symmetric_cipher *aws_aes_keywrap_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key);

/**
 * Cleans up internal resources and state for cipher and then deallocates it.
 */
AWS_CAL_API void aws_symmetric_cipher_destroy(struct aws_symmetric_cipher *cipher);

/**
 * Encrypts the value in to_encrypt and writes the encrypted data into out.
 * If out is dynamic it will be expanded. If it is not, and out is not large enough to handle
 * the encrypted output, the call will fail. If you're trying to optimize to use a stack based array
 * or something, make sure it's at least as large as the size of to_encrypt + an extra BLOCK to account for
 * padding etc...
 *
 * returns AWS_OP_SUCCESS on success. Call aws_last_error() to determine the failure cause if it returns
 * AWS_OP_ERR;
 */
AWS_CAL_API int aws_symmetric_cipher_encrypt(
    struct aws_symmetric_cipher *cipher,
    struct aws_byte_cursor to_encrypt,
    struct aws_byte_buf *out);

/**
 * Decrypts the value in to_decrypt and writes the decrypted data into out.
 * If out is dynamic it will be expanded. If it is not, and out is not large enough to handle
 * the decrypted output, the call will fail. If you're trying to optimize to use a stack based array
 * or something, make sure it's at least as large as the size of to_decrypt + an extra BLOCK to account for
 * padding etc...
 *
 * returns AWS_OP_SUCCESS on success. Call aws_last_error() to determine the failure cause if it returns
 * AWS_OP_ERR;
 */
AWS_CAL_API int aws_symmetric_cipher_decrypt(
    struct aws_symmetric_cipher *cipher,
    struct aws_byte_cursor to_decrypt,
    struct aws_byte_buf *out);

/**
 * Encrypts any remaining data that was reserved for final padding, loads GMACs etc... and if there is any
 * writes any remaining encrypted data to out. If out is dynamic it will be expanded. If it is not, and
 * out is not large enough to handle the decrypted output, the call will fail. If you're trying to optimize
 *  to use a stack based array or something, make sure it's at least as large as the size of 2 BLOCKs to account for
 * padding etc...
 *
 * After invoking this function, you MUST call aws_symmetric_cipher_reset() before invoking any encrypt/decrypt
 * operations on this cipher again.
 *
 * returns AWS_OP_SUCCESS on success. Call aws_last_error() to determine the failure cause if it returns
 * AWS_OP_ERR;
 */
AWS_CAL_API int aws_symmetric_cipher_finalize_encryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out);

/**
 * Decrypts any remaining data that was reserved for final padding, loads GMACs etc... and if there is any
 * writes any remaining decrypted data to out. If out is dynamic it will be expanded. If it is not, and
 * out is not large enough to handle the decrypted output, the call will fail. If you're trying to optimize
 * to use a stack based array or something, make sure it's at least as large as the size of 2 BLOCKs to account for
 * padding etc...
 *
 * After invoking this function, you MUST call aws_symmetric_cipher_reset() before invoking any encrypt/decrypt
 * operations on this cipher again.
 *
 * returns AWS_OP_SUCCESS on success. Call aws_last_error() to determine the failure cause if it returns
 * AWS_OP_ERR;
 */
AWS_CAL_API int aws_symmetric_cipher_finalize_decryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out);

/**
 * Resets the cipher state for starting a new encrypt or decrypt operation. Note encrypt/decrypt cannot be mixed on the
 * same cipher without a call to reset in between them. However, this leaves the key, iv etc... materials setup for
 * immediate reuse.
 * Note: GCM tag is not preserved between operations. If you intend to do encrypt followed directly by decrypt, make
 * sure to make a copy of tag before reseting the cipher and pass that copy for decryption.
 *
 * Warning: In most cases it's a really bad idea to reset a cipher and perform another operation using that cipher.
 * Key and IV should not be reused for different operations. Instead of reseting the cipher, destroy the cipher
 * and create new one with a new key/iv pair. Use reset at your own risk, and only after careful consideration.
 *
 * returns AWS_OP_SUCCESS on success. Call aws_last_error() to determine the failure cause if it returns
 * AWS_OP_ERR;
 */
AWS_CAL_API int aws_symmetric_cipher_reset(struct aws_symmetric_cipher *cipher);

/**
 * Gets the current GMAC tag. If not AES GCM, this function will just return an empty cursor.
 * The memory in this cursor is unsafe as it refers to the internal buffer.
 * This was done because the use case doesn't require fetching these during an
 * encryption or decryption operation and it dramatically simplifies the API.
 * Only use this function between other calls to this API as any function call can alter the value of this tag.
 *
 * If you need to access it in a different pattern, copy the values to your own buffer first.
 */
AWS_CAL_API struct aws_byte_cursor aws_symmetric_cipher_get_tag(const struct aws_symmetric_cipher *cipher);

/**
 * Sets the GMAC tag on the cipher. Does nothing for ciphers that do not support tag.
 */
AWS_CAL_API void aws_symmetric_cipher_set_tag(struct aws_symmetric_cipher *cipher, struct aws_byte_cursor tag);

/**
 * Gets the original initialization vector as a cursor.
 * The memory in this cursor is unsafe as it refers to the internal buffer.
 * This was done because the use case doesn't require fetching these during an
 * encryption or decryption operation and it dramatically simplifies the API.
 *
 * Unlike some other fields, this value does not change after the inital construction of the cipher.
 *
 * For some algorithms, such as AES Keywrap, this will return an empty cursor.
 */
AWS_CAL_API struct aws_byte_cursor aws_symmetric_cipher_get_initialization_vector(
    const struct aws_symmetric_cipher *cipher);

/**
 * Gets the original key.
 *
 * The memory in this cursor is unsafe as it refers to the internal buffer.
 * This was done because the use case doesn't require fetching these during an
 * encryption or decryption operation and it dramatically simplifies the API.
 *
 * Unlike some other fields, this value does not change after the inital construction of the cipher.
 */
AWS_CAL_API struct aws_byte_cursor aws_symmetric_cipher_get_key(const struct aws_symmetric_cipher *cipher);

/**
 * Returns true if the state of the cipher is good, and otherwise returns false.
 * Most operations, other than aws_symmetric_cipher_reset() will fail if this function is returning false.
 * aws_symmetric_cipher_reset() will reset the state to a good state if possible.
 */
AWS_CAL_API bool aws_symmetric_cipher_is_good(const struct aws_symmetric_cipher *cipher);

/**
 * Retuns the current state of the cipher. Ther state of the cipher can be ready for use, finalized, or has encountered
 * an error. if the cipher is in a finished or error state, it must be reset before further use.
 */
AWS_CAL_API enum aws_symmetric_cipher_state aws_symmetric_cipher_get_state(const struct aws_symmetric_cipher *cipher);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL
#endif /* AWS_CAL_SYMMETRIC_CIPHER_H */
