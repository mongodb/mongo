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
 *
 * ex_encrypt.c
 * 	demonstrates how to use the encryption API.
 */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include "windows_shim.h"
#endif

#include <wiredtiger.h>

#if defined(_lint)
#define	ATOMIC_ADD(v, val)      ((v) += (val), (v))
#elif defined(_WIN32)
#define	ATOMIC_ADD(v, val)      (_InterlockedExchangeAdd(&(v), val) + val)
#else
#define	ATOMIC_ADD(v, val)      __sync_add_and_fetch(&(v), val)
#endif

static const char * const home = NULL;

/*! [encryption example callback implementation] */
typedef struct {
	WT_ENCRYPTOR encryptor;	/* Must come first */
	uint32_t num_calls;	/* Count of calls */
} EX_ENCRYPTOR;

#define	CHKSUM_LEN	4
#define	IV_LEN		16

/*
 * make_chksum --
 *	This is where one would call a checksum function on the encrypted
 *	buffer.  Here we just put random values in it.
 */
static int
make_chksum(uint8_t *dst)
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
 * ROT13 encryption functions.
 */
/*
 * do_rot13 --
 *	Perform rot-13 on the buffer given.
 */
static void
do_rot13(uint8_t *buf, size_t len)
{
	uint32_t i;
	/*
	 * Now rot13
	 */
	for (i = 0; i < len; i++) {
		if (isalpha(buf[i])) {
			if (tolower(buf[i]) < 'n')
				buf[i] += 13;
			else
				buf[i] -= 13;
		}
	}
}

/*
 * rot13_decrypt --
 *	A simple rot13 decryption.
 */
static int
rot13_decrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;
	uint32_t i;

	(void)session;		/* Unused */
	++ex_encryptor->num_calls;

	if (src == NULL)
		return (0);
	/*
	 * Make sure it is big enough.
	 */
	if (dst_len < src_len - CHKSUM_LEN - IV_LEN)
		return (ENOMEM);

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
	 * Call common rot13 function on the text portion of the
	 * buffer.  Send in dst_len as the length of the text.
	 */
	/*
	 * !!! Most implementations would need the IV too.
	 */
	do_rot13(&dst[0], dst_len);
	*result_lenp = dst_len;
	return (0);
}

/*
 * rot13_encrypt --
 *	A simple rot13 encryption.
 */
static int
rot13_encrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;
	uint32_t i;

	(void)session;		/* Unused */
	++ex_encryptor->num_calls;

	if (src == NULL)
		return (0);
	if (dst_len < src_len + CHKSUM_LEN + IV_LEN)
		return (ENOMEM);

	i = CHKSUM_LEN + IV_LEN;
	memcpy(&dst[i], &src[0], src_len);
	/*
	 * Call common rot13 function on the text portion of the
	 * destination buffer.  Send in src_len as the length of
	 * the text.
	 */
	do_rot13(&dst[i], src_len);
	/*
	 * Checksum the encrypted buffer and add the IV.
	 */
	i = 0;
	make_chksum(&dst[i]);
	i += CHKSUM_LEN;
	make_iv(&dst[i]);
	*result_lenp = dst_len;
	return (0);
}

/*
 * rot13_post_size --
 *	A simple post-size example that returns the source length.
 */
static int
rot13_post_size(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;

	(void)session;				/* Unused parameters */
	(void)src;

	++ex_encryptor->num_calls;		/* Call count */

	*result_lenp = src_len - CHKSUM_LEN - IV_LEN;
	return (0);
}

/*
 * rot13_pre_size --
 *	A simple pre-size example that returns the source length.
 */
static int
rot13_pre_size(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;

	(void)session;				/* Unused parameters */
	(void)src;

	++ex_encryptor->num_calls;		/* Call count */

	*result_lenp = src_len + CHKSUM_LEN + IV_LEN;
	return (0);
}

/*
 * rot13_terminate --
 *	WiredTiger rot13 encryption termination.
 */
static int
rot13_terminate(WT_ENCRYPTOR *encryptor, WT_SESSION *session)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;

	(void)session;				/* Unused parameters */

	++ex_encryptor->num_calls;		/* Call count */

	/* Free the allocated memory. */
	free(encryptor);

	return (0);
}

/*
 * XOR encryption functions.
 */
/*
 * do_bitwisenot --
 *	Perform XOR on the buffer given.
 */
static void
do_bitwisenot(uint8_t *buf, size_t len)
{
	uint32_t i;
	/*
	 * Now bitwise not
	 */
	for (i = 0; i < len; i++)
		buf[i] = ~buf[i];
}

/*
 * not_decrypt --
 *	A simple not decryption.
 */
static int
not_decrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;
	uint32_t i;

	(void)session;		/* Unused */
	++ex_encryptor->num_calls;

	if (src == NULL)
		return (0);
	/*
	 * Make sure it is big enough.
	 */
	if (dst_len < src_len - CHKSUM_LEN - IV_LEN)
		return (ENOMEM);

	/*
	 * Most implementations would verify the checksum now.
	 */
	i = CHKSUM_LEN + IV_LEN;
	memcpy(&dst[0], &src[i], dst_len);
	/*
	 * Call common not function on the text portion of the
	 * source buffer.  Send in dst_len as the length of
	 * the text.
	 */
	/*
	 * Most implementations would send in the IV too.
	 */
	do_bitwisenot(&dst[0], dst_len);
	*result_lenp = dst_len;
	return (0);
}

/*
 * not_encrypt --
 *	A simple not encryption.
 */
static int
not_encrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;
	uint32_t i;

	(void)session;		/* Unused */
	++ex_encryptor->num_calls;

	if (src == NULL)
		return (0);
	if (dst_len < src_len + CHKSUM_LEN + IV_LEN)
		return (ENOMEM);

	i = CHKSUM_LEN + IV_LEN;
	memcpy(&dst[i], &src[0], src_len);
	/*
	 * Call common not function on the text portion of the
	 * destination buffer.  Send in src_len as the length of
	 * the text.
	 */
	do_bitwisenot(&dst[i], src_len);
	/*
	 * Checksum the encrypted buffer and add the IV.
	 */
	i = 0;
	make_chksum(&dst[i]);
	i += CHKSUM_LEN;
	make_iv(&dst[i]);
	*result_lenp = dst_len;
	return (0);
}

/*
 * not_post_size --
 *	A simple post-size example that returns the source length.
 */
static int
not_post_size(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;

	(void)session;				/* Unused parameters */
	(void)src;

	++ex_encryptor->num_calls;		/* Call count */

	*result_lenp = src_len - CHKSUM_LEN - IV_LEN;
	return (0);
}

/*
 * not_pre_size --
 *	A simple pre-size example that returns the source length.
 */
static int
not_pre_size(WT_ENCRYPTOR *encryptor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;

	(void)session;				/* Unused parameters */
	(void)src;

	++ex_encryptor->num_calls;		/* Call count */

	*result_lenp = src_len + CHKSUM_LEN + IV_LEN;
	return (0);
}

/*
 * not_terminate --
 *	WiredTiger not encryption termination.
 */
static int
not_terminate(WT_ENCRYPTOR *encryptor, WT_SESSION *session)
{
	EX_ENCRYPTOR *ex_encryptor = (EX_ENCRYPTOR *)encryptor;

	(void)session;				/* Unused parameters */

	++ex_encryptor->num_calls;		/* Call count */

	/* Free the allocated memory. */
	free(encryptor);

	return (0);
}

/*
 * add_my_encryptors --
 *	A simple example of adding encryption callbacks.
 */
int
add_my_encryptors(WT_CONNECTION *connection)
{
	EX_ENCRYPTOR *not_encryptor, *rot13_encryptor;
	int ret;

	if ((not_encryptor = calloc(1, sizeof(EX_ENCRYPTOR))) == NULL)
		return (errno);

	if ((rot13_encryptor = calloc(1, sizeof(EX_ENCRYPTOR))) == NULL)
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
	not_encryptor->encryptor.encrypt = not_encrypt;
	not_encryptor->encryptor.decrypt = not_decrypt;
	not_encryptor->encryptor.post_size = not_post_size;
	not_encryptor->encryptor.pre_size = not_pre_size;
	not_encryptor->encryptor.terminate = not_terminate;

	if ((ret = connection->add_encryptor(
	    connection, "not", (WT_ENCRYPTOR *)not_encryptor, NULL)) != 0)
		return (ret);

	rot13_encryptor->encryptor.encrypt = rot13_encrypt;
	rot13_encryptor->encryptor.decrypt = rot13_decrypt;
	rot13_encryptor->encryptor.post_size = rot13_post_size;
	rot13_encryptor->encryptor.pre_size = rot13_pre_size;
	rot13_encryptor->encryptor.terminate = rot13_terminate;

	return (connection->add_encryptor(
	    connection, "rot13", (WT_ENCRYPTOR *)rot13_encryptor, NULL));
}

#define	MAX_KEYS	100

int
main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *c1, *c2;
	int i, ret;
	char k[16], v[16];

	srandom((unsigned int)getpid());

	ret = wiredtiger_open(home, NULL,
	    "create,cache_size=100MB,"
	    "log=(enabled=true,encryption_algorithm=not,"
	    "encryption_password=xyz)", &conn);

	ret = add_my_encryptors(conn);

	ret = conn->open_session(conn, NULL, NULL, &session);
	ret = session->create(session, "table:crypto",
	    "encryption=(enabled=true,algorithm=rot13,"
	    "password=abc123),key_format=S,value_format=S");
	ret = session->create(
	    session, "table:nocrypto",
	    "key_format=S,value_format=S");

	/* Insert a set of keys */
	ret = session->open_cursor(session, "table:crypto", NULL, NULL, &c1);
	ret = session->open_cursor(session, "table:nocrypto", NULL, NULL, &c2);

	for (i = 0; i < MAX_KEYS; i++) {
		snprintf(k, sizeof(k), "key%d", i);
		c1->set_key(c1, k);
		c2->set_key(c2, k);

		snprintf(v, sizeof(v), "value%d", i);
		c1->set_value(c1, v);
		c2->set_value(c2, v);

		ret = c1->insert(c1);
		ret = c2->insert(c2);
	}

	c1->reset(c1);
	c2->reset(c2);
	while (c1->next(c1) == 0) {
		ret = c1->get_key(c1, &k);
		ret = c1->get_value(c1, &v);

		printf("Read key %s; value %s\n", k, v);
	}
	ret = conn->close(conn, NULL);

	return (ret);
}
