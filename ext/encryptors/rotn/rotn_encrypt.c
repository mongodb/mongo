/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/*! [WT_ENCRYPTOR initialization structure] */
/* Local encryptor structure. */
typedef struct {
	WT_ENCRYPTOR encryptor;	/* Must come first */
	WT_EXTENSION_API *wt_api; /* Extension API */
	uint32_t rot_N;		/* rotN value */
	char *keyid;		/* Saved keyid */
	char *password;		/* Saved password */

} ROTN_ENCRYPTOR;
/*! [WT_ENCRYPTOR initialization structure] */

#define	CHKSUM_LEN	4
#define	IV_LEN		16


/*
 * make_cksum --
 *	This is where one would call a checksum function on the encrypted
 *	buffer.  Here we just put random values in it.
 */
static int
make_cksum(uint8_t *dst)
{
	int i;
	/*
	 * Assume array is big enough for the checksum.
	 */
	for (i = 0; i < CHKSUM_LEN; i++)
		dst[i] = (uint8_t)random();
	return (0);
}

/*
 * make_iv --
 *	This is where one would generate the initialization vector.
 *	Here we just put random values in it.
 */
static int
make_iv(uint8_t *dst)
{
	int i;
	/*
	 * Assume array is big enough for the initialization vector.
	 */
	for (i = 0; i < IV_LEN; i++)
		dst[i] = (uint8_t)random();
	return (0);
}

/*
 * Rotate encryption functions.
 */
/*
 * do_rotate --
 *	Perform rot-N on the buffer given.
 */
static void
do_rotate(uint8_t *buf, size_t len, uint32_t rotn)
{
	uint32_t i;
	/*
	 * Now rotate
	 */
	for (i = 0; i < len; i++) {
		if (isalpha(buf[i])) {
			if (islower(buf[i]))
				buf[i] = (buf[i] - 'a' + rotn) % 26 + 'a';
			else
				buf[i] = (buf[i] - 'A' + rotn) % 26 + 'A';
		}
	}
}

/*! [WT_ENCRYPTOR encrypt] */
/*
 * rotn_encrypt --
 *	A simple encryption example that passes data through unchanged.
 */
static int
rotn_encrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	ROTN_ENCRYPTOR *rotn_encryptor = (ROTN_ENCRYPTOR *)encryptor;
	uint32_t i;

	(void)session;		/* Unused */

	if (src == NULL)
		return (0);
	if (dst_len < src_len + CHKSUM_LEN + IV_LEN)
		return (ENOMEM);

	i = CHKSUM_LEN + IV_LEN;
	memcpy(&dst[i], &src[0], src_len);
	/*
	 * Call common rotate function on the text portion of the
	 * destination buffer.  Send in src_len as the length of
	 * the text.
	 */
	do_rotate(&dst[i], src_len, rotn_encryptor->rot_N);
	/*
	 * Checksum the encrypted buffer and add the IV.
	 */
	i = 0;
	make_cksum(&dst[i]);
	i += CHKSUM_LEN;
	make_iv(&dst[i]);
	*result_lenp = dst_len;
	return (0);
}
/*! [WT_ENCRYPTOR encrypt] */

/*! [WT_ENCRYPTOR decrypt] */
/*
 * rotn_decrypt --
 *	A simple decryption example that passes data through unchanged.
 */
static int
rotn_decrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	ROTN_ENCRYPTOR *rotn_encryptor = (ROTN_ENCRYPTOR *)encryptor;
	uint32_t i;

	(void)session;		/* Unused */

	if (src == NULL)
		return (0);
	/*
	 * Make sure it is big enough.
	 */
	if (dst_len < src_len - CHKSUM_LEN - IV_LEN) {
		fprintf(stderr, "Rotate: ENOMEM ERROR\n");
		return (ENOMEM);
	}

	/*
	 * !!! Most implementations would verify the checksum here.
	 */
	/*
	 * Copy the encrypted data to the destination buffer and then
	 * decrypt the destination buffer.
	 */
	i = CHKSUM_LEN + IV_LEN;
	memcpy(&dst[0], &src[i], dst_len);
	/*
	 * Call common rotate function on the text portion of the
	 * buffer.  Send in dst_len as the length of the text.
	 */
	/*
	 * !!! Most implementations would need the IV too.
	 */
	do_rotate(&dst[0], dst_len, 26 - rotn_encryptor->rot_N);
	*result_lenp = dst_len;
	return (0);
}
/*! [WT_ENCRYPTOR decrypt] */

/*! [WT_ENCRYPTOR postsize] */
/*
 * rotn_sizing --
 *	A sizing example that returns the header size needed.
 */
static int
rotn_sizing(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    size_t *expansion_constantp)
{
	(void)encryptor;				/* Unused parameters */
	(void)session;				/* Unused parameters */

	*expansion_constantp = CHKSUM_LEN + IV_LEN;
	return (0);
}
/*! [WT_ENCRYPTOR postsize] */

/*! [WT_ENCRYPTOR customize] */
/*
 * rotn_customize --
 *	The customize function creates a customized encryptor
 */
static int
rotn_customize(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    WT_CONFIG_ARG *encrypt_config, WT_ENCRYPTOR **customp)
{
	int ret, rotamt;
	const ROTN_ENCRYPTOR *orig;
	ROTN_ENCRYPTOR *rotn_encryptor;
	WT_CONFIG_ITEM keyid, secret;
	WT_EXTENSION_API *wt_api;

	ret = 0;

	orig = (const ROTN_ENCRYPTOR *)encryptor;
	wt_api = orig->wt_api;

	if ((rotn_encryptor = calloc(1, sizeof(ROTN_ENCRYPTOR))) == NULL)
		return (errno);
	*rotn_encryptor = *orig;
	rotn_encryptor->keyid = rotn_encryptor->password = NULL;

	/*
	 * Stash the keyid and the (optional) secret key
	 * from the configuration string.
	 */
	if ((ret = wt_api->config_get(wt_api, session, encrypt_config,
	    "keyid", &keyid)) == 0 && keyid.len != 0) {
		if ((rotn_encryptor->keyid = malloc(keyid.len + 1)) == NULL) {
			ret = errno;
			goto err;
		}
		strncpy(rotn_encryptor->keyid, keyid.str, keyid.len + 1);
		rotn_encryptor->keyid[keyid.len] = '\0';
	}

	if ((ret = wt_api->config_get(wt_api, session, encrypt_config,
	    "secretkey", &secret)) == 0 && secret.len != 0) {
		if ((rotn_encryptor->password = malloc(secret.len + 1))
		    == NULL) {
			ret = errno;
			goto err;
		}
		strncpy(rotn_encryptor->password, secret.str, secret.len + 1);
		rotn_encryptor->password[secret.len] = '\0';
	}
	/*
	 * Presumably we'd have some sophisticated key management
	 * here that maps the id onto a secret key.
	 */
	if ((rotamt = atoi(keyid.str)) < 0) {
		ret = EINVAL;
		goto err;
	}
	rotn_encryptor->rot_N = rotamt;

	*customp = &rotn_encryptor->encryptor;
	return (0);
err:
	if (rotn_encryptor->keyid != NULL)
		free(rotn_encryptor->keyid);
	if (rotn_encryptor->password != NULL)
		free(rotn_encryptor->password);
	free(rotn_encryptor);
	return (EPERM);
}
/*! [WT_ENCRYPTOR presize] */

/*! [WT_ENCRYPTOR terminate] */
/*
 * rotn_terminate --
 *	WiredTiger no-op encryption termination.
 */
static int
rotn_terminate(WT_ENCRYPTOR *encryptor, WT_SESSION *session)
{
	ROTN_ENCRYPTOR *rotn_encryptor = (ROTN_ENCRYPTOR *)encryptor;

	(void)session;				/* Unused parameters */

	/* Free the allocated memory. */
	if (rotn_encryptor->password != NULL) {
		free(rotn_encryptor->password);
		rotn_encryptor->password = NULL;
	}
	if (rotn_encryptor->keyid != NULL) {
		free(rotn_encryptor->keyid);
		rotn_encryptor->keyid = NULL;
	}

	free(encryptor);

	return (0);
}
/*! [WT_ENCRYPTOR terminate] */

/*! [WT_ENCRYPTOR initialization function] */
/*
 * wiredtiger_extension_init --
 *	A simple shared library encryption example.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	ROTN_ENCRYPTOR *rotn_encryptor;

	(void)config;				/* Unused parameters */

	if ((rotn_encryptor = calloc(1, sizeof(ROTN_ENCRYPTOR))) == NULL)
		return (errno);

	/*
	 * Allocate a local encryptor structure, with a WT_ENCRYPTOR structure
	 * as the first field, allowing us to treat references to either type of
	 * structure as a reference to the other type.
	 *
	 * This could be simplified if only a single database is opened in the
	 * application, we could use a static WT_ENCRYPTOR structure, and a
	 * static reference to the WT_EXTENSION_API methods, then we don't need
	 * to allocate memory when the encryptor is initialized or free it when
	 * the encryptor is terminated.  However, this approach is more general
	 * purpose and supports multiple databases per application.
	 */
	rotn_encryptor->encryptor.encrypt = rotn_encrypt;
	rotn_encryptor->encryptor.decrypt = rotn_decrypt;
	rotn_encryptor->encryptor.sizing = rotn_sizing;
	rotn_encryptor->encryptor.customize = rotn_customize;
	rotn_encryptor->encryptor.terminate = rotn_terminate;

	rotn_encryptor->wt_api = connection->get_extension_api(connection);

						/* Load the encryptor */
	return (connection->add_encryptor(
	    connection, "rotn", (WT_ENCRYPTOR *)rotn_encryptor, NULL));
}
/*! [WT_ENCRYPTOR initialization function] */
