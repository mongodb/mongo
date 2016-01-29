/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/*! [WT_ENCRYPTOR initialization structure] */

/*
 * This encryptor is used for testing and demonstration only.
 *
 * IT IS TRIVIAL TO BREAK AND DOES NOT OFFER ANY SECURITY!
 *
 * There are two configuration parameters that control it: the keyid and the
 * secretkey (which may be thought of as a password).  The keyid is expected
 * to be a digits giving a number between 0 and 25.  The secretkey, when
 * present, must be composed of alphabetic characters.
 *
 * When there is no secretkey, the encryptor acts as a ROT(N) encryptor (a
 * "Caesar cipher"), where N is the value of keyid.  Thus, with keyid=13,
 * text "Hello" maps to "Uryyb", as we preserve case.  Only the alphabetic
 * characters in the input text are changed.
 *
 * When there is a secretkey we are implementing a Vigenere cipher.
 * Each byte is rotated the distance from 'A' for each letter in the
 * (repeating) secretkey.  The distance is increased by the value of
 * the keyid.  Thus, with secretkey "ABC" and keyid "2", we show how
 * we map the input "MySecret".
 *    secretkey          = ABC
 *    distances from 'A' = 012
 *    add keyid (2)      = 234
 *    repeated           = 23423423
 *    input              = MySecret
 *    output             = ObWgfvgw
 * In this case, we transform all bytes in the input.
 */

/* Local encryptor structure. */
typedef struct {
	WT_ENCRYPTOR encryptor;		/* Must come first */

	WT_EXTENSION_API *wtext;	/* Extension API */

	int rot_N;			/* rotN value */
	char *keyid;			/* Saved keyid */
	char *secretkey;		/* Saved secretkey */
	u_char *shift_forw;		/* Encrypt shift data from secretkey */
	u_char *shift_back;		/* Decrypt shift data from secretkey */
	size_t shift_len;		/* Length of shift* byte arrays */
	int force_error;		/* Force a decrypt error for testing */

} ROTN_ENCRYPTOR;
/*! [WT_ENCRYPTOR initialization structure] */

#define	CHKSUM_LEN	4
#define	IV_LEN		16

/*
 * rotn_error --
 *	Display an error from this module in a standard way.
 */
static int
rotn_error(ROTN_ENCRYPTOR *encryptor, WT_SESSION *session, int err,
    const char *msg)
{
	WT_EXTENSION_API *wtext;

	wtext = encryptor->wtext;
	(void)wtext->err_printf(wtext, session,
	    "rotn encryption: %s: %s", msg, wtext->strerror(wtext, NULL, err));
	return (err);
}

/*
 * make_cksum --
 *	This is where one would call a checksum function on the encrypted
 *	buffer.  Here we just put a constant value in it.
 */
static void
make_cksum(uint8_t *dst)
{
	int i;
	/*
	 * Assume array is big enough for the checksum.
	 */
	for (i = 0; i < CHKSUM_LEN; i++)
		dst[i] = 'C';
}

/*
 * make_iv --
 *	This is where one would generate the initialization vector.
 *	Here we just put a constant value in it.
 */
static void
make_iv(uint8_t *dst)
{
	int i;
	/*
	 * Assume array is big enough for the initialization vector.
	 */
	for (i = 0; i < IV_LEN; i++)
		dst[i] = 'I';
}

/*
 * Rotate encryption functions.
 */
/*
 * do_rotate --
 *	Perform rot-N on the buffer given.
 */
static void
do_rotate(char *buf, size_t len, int rotn)
{
	uint32_t i;
	/*
	 * Now rotate.
	 *
	 * Avoid ctype functions because they behave in unexpected ways,
	 * particularly when the locale is not "C".
	 */
	for (i = 0; i < len; i++) {
		if ('a' <= buf[i] && buf[i] <= 'z')
			buf[i] = ((buf[i] - 'a') + rotn) % 26 + 'a';
		else if ('A' <= buf[i] && buf[i] <= 'Z')
			buf[i] = ((buf[i] - 'A') + rotn) % 26 + 'A';
	}
}

/*
 * do_shift --
 *	Perform a Vigenere cipher
 */
static void
do_shift(uint8_t *buf, size_t len, u_char *shift, size_t shiftlen)
{
	uint32_t i;
	/*
	 * Now shift.
	 */
	for (i = 0; i < len; i++)
		buf[i] += shift[i % shiftlen];
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

	if (dst_len < src_len + CHKSUM_LEN + IV_LEN)
		return (ENOMEM);

	/*
	 * !!! Most implementations would verify any needed
	 * checksum and initialize the IV here.
	 */
	i = CHKSUM_LEN + IV_LEN;
	memcpy(&dst[i], &src[0], src_len);
	/*
	 * Depending on whether we have a secret key or not,
	 * call the common rotate or shift function on the text portion
	 * of the destination buffer.  Send in src_len as the length of
	 * the text.
	 */
	if (rotn_encryptor->shift_len == 0)
		do_rotate((char *)dst + i, src_len, rotn_encryptor->rot_N);
	else
		do_shift(&dst[i], src_len,
		    rotn_encryptor->shift_forw, rotn_encryptor->shift_len);
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
	size_t mylen;
	uint32_t i;

	(void)session;		/* Unused */

	/*
	 * For certain tests, force an error we can recognize.
	 */
	if (rotn_encryptor->force_error)
		return (-1000);

	/*
	 * Make sure it is big enough.
	 */
	mylen = src_len - (CHKSUM_LEN + IV_LEN);
	if (dst_len < mylen)
		return (rotn_error(rotn_encryptor, session,
		    ENOMEM, "decrypt buffer not big enough"));

	/*
	 * !!! Most implementations would verify the checksum here.
	 */
	/*
	 * Copy the encrypted data to the destination buffer and then
	 * decrypt the destination buffer.
	 */
	i = CHKSUM_LEN + IV_LEN;
	memcpy(&dst[0], &src[i], mylen);
	/*
	 * Depending on whether we have a secret key or not,
	 * call the common rotate or shift function on the text portion
	 * of the destination buffer.  Send in dst_len as the length of
	 * the text.
	 */
	/*
	 * !!! Most implementations would need the IV too.
	 */
	if (rotn_encryptor->shift_len == 0)
		do_rotate((char *)dst, mylen, 26 - rotn_encryptor->rot_N);
	else
		do_shift(&dst[0], mylen,
		    rotn_encryptor->shift_back, rotn_encryptor->shift_len);
	*result_lenp = mylen;
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
	const ROTN_ENCRYPTOR *orig;
	ROTN_ENCRYPTOR *rotn_encryptor;
	WT_CONFIG_ITEM keyid, secret;
	WT_EXTENSION_API *wtext;
	size_t i, len;
	int ret, keyid_val;
	u_char base;

	ret = 0;
	keyid_val = 0;

	orig = (const ROTN_ENCRYPTOR *)encryptor;
	wtext = orig->wtext;

	if ((rotn_encryptor = calloc(1, sizeof(ROTN_ENCRYPTOR))) == NULL)
		return (errno);
	*rotn_encryptor = *orig;
	rotn_encryptor->keyid = rotn_encryptor->secretkey = NULL;

	/*
	 * Stash the keyid from the configuration string.
	 */
	if ((ret = wtext->config_get(wtext, session, encrypt_config,
	    "keyid", &keyid)) == 0 && keyid.len != 0) {
		/*
		 * In this demonstration, we expect keyid to be a number.
		 */
		if ((keyid_val = atoi(keyid.str)) < 0) {
			ret = EINVAL;
			goto err;
		}
		if ((rotn_encryptor->keyid = malloc(keyid.len + 1)) == NULL) {
			ret = errno;
			goto err;
		}
		strncpy(rotn_encryptor->keyid, keyid.str, keyid.len + 1);
		rotn_encryptor->keyid[keyid.len] = '\0';
	}

	/*
	 * In this demonstration, the secret key must be alphabetic characters.
	 * We stash the secret key from the configuration string
	 * and build some shift bytes to make encryption/decryption easy.
	 */
	if ((ret = wtext->config_get(wtext, session, encrypt_config,
	    "secretkey", &secret)) == 0 && secret.len != 0) {
		len = secret.len;
		if ((rotn_encryptor->secretkey = malloc(len + 1)) == NULL ||
		    (rotn_encryptor->shift_forw = malloc(len)) == NULL ||
		    (rotn_encryptor->shift_back = malloc(len)) == NULL) {
			ret = errno;
			goto err;
		}
		for (i = 0; i < len; i++) {
			if ('a' <= secret.str[i] && secret.str[i] <= 'z')
				base = 'a';
			else if ('A' <= secret.str[i] && secret.str[i] <= 'Z')
				base = 'A';
			else {
				ret = EINVAL;
				goto err;
			}
			base -= (u_char)keyid_val;
			rotn_encryptor->shift_forw[i] =
			    (u_char)secret.str[i] - base;
			rotn_encryptor->shift_back[i] =
			    base - (u_char)secret.str[i];
		}
		rotn_encryptor->shift_len = len;
		strncpy(rotn_encryptor->secretkey, secret.str, secret.len + 1);
		rotn_encryptor->secretkey[secret.len] = '\0';
	}

	/*
	 * In a real encryptor, we could use some sophisticated key management
	 * here to map the keyid onto a secret key.
	 */
	rotn_encryptor->rot_N = keyid_val;

	*customp = (WT_ENCRYPTOR *)rotn_encryptor;
	return (0);

err:	free(rotn_encryptor->keyid);
	free(rotn_encryptor->secretkey);
	free(rotn_encryptor->shift_forw);
	free(rotn_encryptor->shift_back);
	free(rotn_encryptor);
	return (ret);
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
	free(rotn_encryptor->secretkey);
	free(rotn_encryptor->keyid);
	free(rotn_encryptor->shift_forw);
	free(rotn_encryptor->shift_back);
	free(encryptor);
	return (0);
}
/*! [WT_ENCRYPTOR terminate] */

/*
 * rotn_configure --
 *	WiredTiger no-op encryption configuration.
 */
static int
rotn_configure(ROTN_ENCRYPTOR *rotn_encryptor, WT_CONFIG_ARG *config)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *config_parser;
	WT_EXTENSION_API *wtext;	/* Extension API */
	int ret, t_ret;

	wtext = rotn_encryptor->wtext;

	/* Get the configuration string. */
	if ((ret = wtext->config_get(wtext, NULL, config, "config", &v)) != 0)
		return (rotn_error(rotn_encryptor, NULL, ret,
		    "WT_EXTENSION_API.config_get"));

	/* Step through the list of configuration options. */
	if ((ret = wtext->config_parser_open(
	    wtext, NULL, v.str, v.len, &config_parser)) != 0)
		return (rotn_error(rotn_encryptor, NULL, ret,
		    "WT_EXTENSION_API.config_parser_open"));

	while ((ret = config_parser->next(config_parser, &k, &v)) == 0) {
		if (strncmp("rotn_force_error", k.str, k.len) == 0 &&
		    strlen("rotn_force_error") == k.len) {
			rotn_encryptor->force_error = v.val == 0 ? 0 : 1;
			continue;
		}
		else {
			(void)config_parser->close(config_parser);
			return (rotn_error(rotn_encryptor, NULL, EINVAL,
			    "unknown config key"));
		}
	}
	if ((t_ret = config_parser->close(config_parser)) != 0)
		return (rotn_error(rotn_encryptor, NULL, t_ret,
		    "WT_CONFIG_PARSER.close"));
	if (ret != WT_NOTFOUND)
		return (rotn_error(rotn_encryptor, NULL, ret,
		    "WT_CONFIG_PARSER.next"));

	return (0);
}

/*! [WT_ENCRYPTOR initialization function] */
/*
 * wiredtiger_extension_init --
 *	A simple shared library encryption example.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	ROTN_ENCRYPTOR *rotn_encryptor;
	int ret;

	if ((rotn_encryptor = calloc(1, sizeof(ROTN_ENCRYPTOR))) == NULL)
		return (errno);

	/*
	 * Allocate a local encryptor structure, with a WT_ENCRYPTOR structure
	 * as the first field, allowing us to treat references to either type of
	 * structure as a reference to the other type.
	 *
	 * Heap memory (not static), because it can support multiple databases.
	 */
	rotn_encryptor->encryptor.encrypt = rotn_encrypt;
	rotn_encryptor->encryptor.decrypt = rotn_decrypt;
	rotn_encryptor->encryptor.sizing = rotn_sizing;
	rotn_encryptor->encryptor.customize = rotn_customize;
	rotn_encryptor->encryptor.terminate = rotn_terminate;
	rotn_encryptor->wtext = connection->get_extension_api(connection);

	if ((ret = rotn_configure(rotn_encryptor, config)) != 0) {
		free(rotn_encryptor);
		return (ret);
	}
						/* Load the encryptor */
	return (connection->add_encryptor(
	    connection, "rotn", (WT_ENCRYPTOR *)rotn_encryptor, NULL));
}
/*! [WT_ENCRYPTOR initialization function] */
