/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Encryption extension using libsodium for cryptography.
 *
 * Note: we recommend that all applications relying on encryption for security audit their
 * application and storage toolchain code, including this implementation and the underlying
 * cryptographic libraries.
 *
 * The only threat model this extension is intended to protect against is: your database was on your
 * laptop, and your laptop was stolen while the database was at rest (shut down).
 *
 * Because the key is necessarily kept in memory while the database is running, it is important to
 * make sure it does not get written to disk by OS mechanisms; for example, core dumps and kernel
 * crash dumps should be disabled, and swapping should be either disabled or set up to be encrypted.
 * Also note that a still-running database on a laptop that is suspended is not at rest.
 *
 * This code does not, for the moment, support any form of external key server or key management
 * service. Keys must be configured with "secretkey=" at database open time, and not with "keyid=".
 * This is workable, but less than optimal. To add support for your favorite key service, copy this
 * file, edit where indicated below, and install as your own custom extension. (See the other
 * encryption examples for further information about how to do this.)
 *
 * The secretkey= configured at database open time is expected to be a 256-bit chacha20 key, printed
 * as hex (with no leading 0x), thus 64 characters long. If you want to use a passphrase instead,
 * use the recommended tools in libsodium to generate a key from a passphrase and pass the results
 * as the secretkey= configuration.
 */

#include <errno.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#include <sodium.h>

/*
 * Lengths of the pieces involved: the secret key, the non-secret but unique nonce, and the
 * cryptographic checksum added as part of the encryption.
 */
#define CHECK_LEN crypto_aead_xchacha20poly1305_ietf_ABYTES
#define KEY_LEN crypto_aead_xchacha20poly1305_ietf_KEYBYTES
#define NONCE_LEN crypto_aead_xchacha20poly1305_ietf_NPUBBYTES

/*
 * We write a header on each block that records the expected format version and the cryptographic
 * construction used. This is incorporated into the block's cryptographic checksum and thus
 * protected from interference. The header is 4 bytes long to keep the following data aligned, but
 * we only use two of them. Currently we only support the one construction, but that could
 * conceivably change and in any event it's a good idea to make persistent formats extensible.
 */
#define HEADER_BYTE_FORMATVERSION 0
#define HEADER_BYTE_CONSTRUCTION 1
#define HEADER_BYTE_ZERO_0 2
#define HEADER_BYTE_ZERO_1 3
#define HEADER_LEN 4

/* Constants for the on-disk (output) format. */
#define ONDISK_CIPHER_XCHACHA20POLY1305 1
#define ONDISK_VERSION_CURRENT 1

/*
 * Each output block contains, in order:
 *    - the header
 *    - the nonce
 *    - the cryptographic output (ciphertext and checksum)
 * The header and nonce are not secret but are covered by the checksum.
 */
#define HEADER_LOCATION 0
#define NONCE_LOCATION HEADER_LEN
#define CIPHERTEXT_LOCATION (HEADER_LEN + NONCE_LEN)

/*
 * Data for this extension. Note that the secret key has to be kept in memory for use.
 */
typedef struct {
    WT_ENCRYPTOR encryptor;   /* Must come first */
    WT_EXTENSION_API *wt_api; /* Extension API */

    uint8_t *secretkey; /* Secret key. (bytes) */
} SODIUM_ENCRYPTOR;

/*
 * sodium_error --
 *     Display an error from this module in a standard way.
 */
static int
sodium_error(SODIUM_ENCRYPTOR *encryptor, WT_SESSION *session, int err, const char *msg)
{
    WT_EXTENSION_API *wt_api;

    wt_api = encryptor->wt_api;
    (void)wt_api->err_printf(
      wt_api, session, "sodium encryption: %s: %s", msg, wt_api->strerror(wt_api, NULL, err));
    return (err);
}

/*
 * create_nonce --
 *     Generate a random nonce.
 */
static void
create_nonce(uint8_t *dst, size_t len)
{
    /*
     * It would be tidier to use incrementing nonces, but currently doing so would require sharing
     * the current nonce between all threads and then doing global locking to use it, which is
     * probably not going to work out that well. There isn't a convenient way to store per-thread
     * extension state.
     */
    randombytes_buf(dst, len);
}

/*
 * sodium_encrypt --
 *     Encrypt using libsodium.
 *
 * Note that the encryption does not require that the input be padded to any particular alignment.
 */
static int
sodium_encrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session, uint8_t *cleartext, size_t clear_len,
  uint8_t *ciphertext, size_t cipher_maxlen, size_t *result_lenp)
{
    SODIUM_ENCRYPTOR *sodium_encryptor = (SODIUM_ENCRYPTOR *)encryptor;
    unsigned long long cipher_len;
    int ret;

    (void)session; /* Unused */

    /* Make sure it is big enough. */
    if (cipher_maxlen < HEADER_LEN + NONCE_LEN + clear_len + CHECK_LEN)
        return (sodium_error(sodium_encryptor, session, ENOMEM, "encrypt buffer not big enough"));

    /* Write the header. */
    ciphertext[HEADER_LOCATION + HEADER_BYTE_FORMATVERSION] = ONDISK_VERSION_CURRENT;
    ciphertext[HEADER_LOCATION + HEADER_BYTE_CONSTRUCTION] = ONDISK_CIPHER_XCHACHA20POLY1305;
    ciphertext[HEADER_LOCATION + HEADER_BYTE_ZERO_0] = 0;
    ciphertext[HEADER_LOCATION + HEADER_BYTE_ZERO_1] = 0;

    /* Make a nonce. */
    create_nonce(&ciphertext[NONCE_LOCATION], NONCE_LEN);

    /* Encrypt and checksum into the ciphertext part of the output. */
    ret = crypto_aead_xchacha20poly1305_ietf_encrypt(&ciphertext[CIPHERTEXT_LOCATION], &cipher_len,
      cleartext, clear_len, &ciphertext[HEADER_LOCATION], HEADER_LEN, NULL,
      &ciphertext[NONCE_LOCATION], sodium_encryptor->secretkey);
    if (ret < 0)
        return (sodium_error(sodium_encryptor, session, WT_ERROR, "encryption failed"));

    *result_lenp = HEADER_LEN + NONCE_LEN + cipher_len;
    return (0);
}

/*
 * sodium_decrypt --
 *     Decrypt using libsodium.
 */
static int
sodium_decrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session, uint8_t *ciphertext, size_t cipher_len,
  uint8_t *cleartext, size_t clear_maxlen, size_t *result_lenp)
{
    SODIUM_ENCRYPTOR *sodium_encryptor = (SODIUM_ENCRYPTOR *)encryptor;
    size_t cipher_check_only_len;
    unsigned long long clear_len;
    int ret;

    /* Make sure it is big enough. */
    if (HEADER_LEN + NONCE_LEN + clear_maxlen + CHECK_LEN < cipher_len)
        return (sodium_error(sodium_encryptor, session, ENOMEM, "decrypt buffer not big enough"));

    /* This is the length of just the ciphertext/checksum part. */
    cipher_check_only_len = cipher_len - HEADER_LEN - NONCE_LEN;

    /* Decrypt and verify the checksum. */
    ret = crypto_aead_xchacha20poly1305_ietf_decrypt(cleartext, &clear_len, NULL,
      &ciphertext[CIPHERTEXT_LOCATION], cipher_check_only_len, &ciphertext[HEADER_LOCATION],
      HEADER_LEN, &ciphertext[NONCE_LOCATION], sodium_encryptor->secretkey);
    if (ret < 0)
        return (sodium_error(sodium_encryptor, session, WT_ERROR, "decryption failed"));

    *result_lenp = clear_len;
    return (0);
}

/*
 * sodium_sizing --
 *     Report how much extra space we need in the output buffer.
 */
static int
sodium_sizing(WT_ENCRYPTOR *encryptor, WT_SESSION *session, size_t *expansion_constantp)
{
    /*
     * Note that the interface assumes the expansion is always a constant; for the construction
     * we're using that's true, but for one based on a block cipher it might need to be rounded up
     * to allow for the ciphertext part of the output always being an integer multiple of the cipher
     * block size.
     */
    (void)encryptor; /* Unused parameters */
    (void)session;   /* Unused parameters */

    /* Expand by the header, the nonce, and the checksum. */
    *expansion_constantp = HEADER_LEN + NONCE_LEN + CHECK_LEN;
    return (0);
}

/*
 * sodium_customize --
 *     The customize function creates a customized encryptor.
 */
static int
sodium_customize(WT_ENCRYPTOR *encryptor, WT_SESSION *session, WT_CONFIG_ARG *encrypt_config,
  WT_ENCRYPTOR **customp)
{
    /*
     * This is how keys are set: the extension is first loaded, and then for every distinct key used
     * a copy is made by calling the customize method. The original uncustomized WT_ENCRYPTOR is
     * never used to encrypt or decrypt anything.
     */
    const SODIUM_ENCRYPTOR *orig;
    SODIUM_ENCRYPTOR *new;
    WT_CONFIG_ITEM keyid, secretkey;
    WT_EXTENSION_API *wt_api;
    size_t keylen;
    int ret;

    orig = (const SODIUM_ENCRYPTOR *)encryptor;
    wt_api = orig->wt_api;

    /* Allocate and initialize the new encryptor. */
    if ((new = calloc(1, sizeof(*new))) == NULL)
        return (errno);
    *new = *orig;
    new->secretkey = NULL;

    /* Get the keyid, if any. */
    ret = wt_api->config_get(wt_api, session, encrypt_config, "keyid", &keyid);
    if (ret != 0)
        keyid.len = 0;

    /* Get the explicit secret key, if any. */
    ret = wt_api->config_get(wt_api, session, encrypt_config, "secretkey", &secretkey);
    if (ret != 0)
        secretkey.len = 0;

    /* Providing both a keyid and a secretkey is an error. */
    if (keyid.len != 0 && secretkey.len != 0) {
        ret = sodium_error(
          new, NULL, EINVAL, "sodium_customize: keys specified with both keyid= and secretkey=");
        goto err;
    }

    /* Providing neither is also an error. */
    if (keyid.len == 0 && secretkey.len == 0) {
        ret = sodium_error(
          new, NULL, EINVAL, "sodium_customize: no key given with either keyid= or secretkey=");
        goto err;
    }

    /* Use sodium_malloc, which takes assorted precautions for working with secrets. */
    new->secretkey = sodium_malloc(KEY_LEN);
    if (new->secretkey == NULL) {
        ret = errno;
        goto err;
    }

    /*
     * We don't support any key lookup services. Yet. To add support for your own, change the code
     * here to fetch the key associated with the configured keyid string, and put it in ->secretkey.
     * If the keyid is invalid, print an error and goto err. Note that there's no need to remember
     * the keyid once the key is installed.
     */
    if (keyid.len != 0) {
        ret = sodium_error(new, NULL, EINVAL, "sodium_customize: keyids not supported yet");
        goto err;
    }

    /*
     * Load the key specified with secretkey=. This should be passed as a 64-character hex string
     * (with no leading 0x). It is not a passphrase; passphrases should be converted to keys using
     * appropriate tools (e.g. those provided by libsodium).
     */
    if (secretkey.len != 0) {
        /* Explicit keys are passed as hex strings of the proper length. */
        ret = sodium_hex2bin(
          new->secretkey, KEY_LEN, secretkey.str, secretkey.len, NULL, &keylen, NULL);
        if (ret < 0) {
            ret = sodium_error(new, NULL, EINVAL, "sodium_customize: secret key not hex");
            goto err;
        }
        if (keylen != KEY_LEN) {
            ret = sodium_error(new, NULL, EINVAL, "sodium_customize: wrong secret key length");
            goto err;
        }
    }

    *customp = (WT_ENCRYPTOR *)new;
    return (0);

err:
    sodium_free(new->secretkey);
    free(new);
    return (ret);
}

/*
 * sodium_terminate --
 *     Shut down the extension and avoid leaking memory.
 */
static int
sodium_terminate(WT_ENCRYPTOR *encryptor, WT_SESSION *session)
{
    SODIUM_ENCRYPTOR *sodium_encryptor = (SODIUM_ENCRYPTOR *)encryptor;

    (void)session; /* Unused parameters */

    /* Close the internal reference in libsodium to /dev/random so it doesn't leak. */
    randombytes_close();

    /* Free the allocated memory. */
    sodium_free(sodium_encryptor->secretkey);
    free(sodium_encryptor);
    return (0);
}

/*
 * sodium_configure --
 *     Configure the extension.
 *
 * This is for static config, not where keys are loaded, so nothing happens.
 */
static int
sodium_configure(SODIUM_ENCRYPTOR *sodium_encryptor, WT_CONFIG_ARG *config)
{
    WT_EXTENSION_API *wt_api; /* Extension API */

    wt_api = sodium_encryptor->wt_api;

    (void)config;
    (void)wt_api;

    return (0);
}

/*
 * wiredtiger_extension_init --
 *     Called to load and initialize the extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    SODIUM_ENCRYPTOR *sodium_encryptor;
    int ret;

    if ((sodium_encryptor = calloc(1, sizeof(SODIUM_ENCRYPTOR))) == NULL)
        return (errno);

    /*
     * Allocate a local encryptor structure, with a WT_ENCRYPTOR structure as the first field,
     * allowing us to treat references to either type of structure as a reference to the other type.
     *
     * Heap memory (not static), because it can support multiple databases.
     */
    sodium_encryptor->encryptor.encrypt = sodium_encrypt;
    sodium_encryptor->encryptor.decrypt = sodium_decrypt;
    sodium_encryptor->encryptor.sizing = sodium_sizing;
    sodium_encryptor->encryptor.customize = sodium_customize;
    sodium_encryptor->encryptor.terminate = sodium_terminate;
    sodium_encryptor->wt_api = connection->get_extension_api(connection);

    /* Initialize the crypto library. */
    if (sodium_init() < 0)
        return (WT_ERROR);

    /* Configure the extension. */
    if ((ret = sodium_configure(sodium_encryptor, config)) != 0) {
        free(sodium_encryptor);
        return (ret);
    }

    /* Attach the extension. */
    if ((ret = connection->add_encryptor(
           connection, "sodium", (WT_ENCRYPTOR *)sodium_encryptor, NULL)) != 0) {
        free(sodium_encryptor);
        return (ret);
    }

    return (0);
}
