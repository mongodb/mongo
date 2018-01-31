/*
 * Go-OpenSSL notice:
 * This file is required for all OpenSSL versions prior to 1.1.0. This simply
 * provides the new 1.1.0 X509_check_* methods for hostname validation if they
 * don't already exist.
 */

#include <openssl/x509.h>

#ifndef X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT

/* portions from x509v3.h and v3_utl.c */
/* Written by Dr Stephen N Henson (steve@openssl.org) for the OpenSSL
 * project.
 */
/* ====================================================================
 * Copyright (c) 1999-2003 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */
/* X509 v3 extension utilities */

#include <string.h>
#include <stdlib.h>
#include <openssl/ssl.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>

#define X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT	0x1
#define X509_CHECK_FLAG_NO_WILDCARDS	0x2

typedef int (*equal_fn)(const unsigned char *pattern, size_t pattern_len,
			const unsigned char *subject, size_t subject_len);

/* Compare while ASCII ignoring case. */
static int equal_nocase(const unsigned char *pattern, size_t pattern_len,
			const unsigned char *subject, size_t subject_len)
	{
	if (pattern_len != subject_len)
		return 0;
	while (pattern_len)
		{
		unsigned char l = *pattern;
		unsigned char r = *subject;
		/* The pattern must not contain NUL characters. */
		if (l == 0)
			return 0;
		if (l != r)
			{
			if ('A' <= l && l <= 'Z')
				l = (l - 'A') + 'a';
			if ('A' <= r && r <= 'Z')
				r = (r - 'A') + 'a';
			if (l != r)
				return 0;
			}
		++pattern;
		++subject;
		--pattern_len;
		}
	return 1;
	}

/* Compare using memcmp. */
static int equal_case(const unsigned char *pattern, size_t pattern_len,
		      const unsigned char *subject, size_t subject_len)
{
	/* The pattern must not contain NUL characters. */
	if (memchr(pattern, '\0', pattern_len) != NULL)
		return 0;
	if (pattern_len != subject_len)
		return 0;
	return !memcmp(pattern, subject, pattern_len);
}

/* RFC 5280, section 7.5, requires that only the domain is compared in
   a case-insensitive manner. */
static int equal_email(const unsigned char *a, size_t a_len,
		       const unsigned char *b, size_t b_len)
	{
	size_t i = a_len;
	if (a_len != b_len)
		return 0;
	/* We search backwards for the '@' character, so that we do
	   not have to deal with quoted local-parts.  The domain part
	   is compared in a case-insensitive manner. */
	while (i > 0)
		{
		--i;
		if (a[i] == '@' || b[i] == '@')
			{
			if (!equal_nocase(a + i, a_len - i,
					  b + i, a_len - i))
				return 0;
			break;
			}
		}
	if (i == 0)
		i = a_len;
	return equal_case(a, i, b, i);
	}

/* Compare the prefix and suffix with the subject, and check that the
   characters in-between are valid. */
static int wildcard_match(const unsigned char *prefix, size_t prefix_len,
			  const unsigned char *suffix, size_t suffix_len,
			  const unsigned char *subject, size_t subject_len)
	{
	const unsigned char *wildcard_start;
	const unsigned char *wildcard_end;
	const unsigned char *p;
	if (subject_len < prefix_len + suffix_len)
		return 0;
	if (!equal_nocase(prefix, prefix_len, subject, prefix_len))
		return 0;
	wildcard_start = subject + prefix_len;
	wildcard_end = subject + (subject_len - suffix_len);
	if (!equal_nocase(wildcard_end, suffix_len, suffix, suffix_len))
		return 0;
	/* The wildcard must match at least one character. */
	if (wildcard_start == wildcard_end)
		return 0;
	/* Check that the part matched by the wildcard contains only
	   permitted characters and only matches a single label. */
	for (p = wildcard_start; p != wildcard_end; ++p)
		if (!(('0' <= *p && *p <= '9') ||
		      ('A' <= *p && *p <= 'Z') ||
		      ('a' <= *p && *p <= 'z') ||
		      *p == '-'))
			return 0;
	return 1;
	}

/* Checks if the memory region consistens of [0-9A-Za-z.-]. */
static int valid_domain_characters(const unsigned char *p, size_t len)
	{
	while (len)
		{
		if (!(('0' <= *p && *p <= '9') ||
		      ('A' <= *p && *p <= 'Z') ||
		      ('a' <= *p && *p <= 'z') ||
		      *p == '-' || *p == '.'))
			return 0;
		++p;
		--len;
		}
	return 1;
	}

/* Find the '*' in a wildcard pattern.  If no such character is found
   or the pattern is otherwise invalid, returns NULL. */
static const unsigned char *wildcard_find_star(const unsigned char *pattern,
					       size_t pattern_len)
	{
	const unsigned char *star = memchr(pattern, '*', pattern_len);
	size_t dot_count = 0;
	const unsigned char *suffix_start;
	size_t suffix_length;
	if (star == NULL)
		return NULL;
	suffix_start = star + 1;
	suffix_length = (pattern + pattern_len) - (star + 1);
	if (!(valid_domain_characters(pattern, star - pattern) &&
	      valid_domain_characters(suffix_start, suffix_length)))
		return NULL;
	/* Check that the suffix matches at least two labels. */
	while (suffix_length)
		{
		if (*suffix_start == '.')
			++dot_count;
		++suffix_start;
		--suffix_length;
		}
	if (dot_count < 2)
		return NULL;
	return star;
	}

/* Compare using wildcards. */
static int equal_wildcard(const unsigned char *pattern, size_t pattern_len,
			  const unsigned char *subject, size_t subject_len)
	{
	const unsigned char *star = wildcard_find_star(pattern, pattern_len);
	if (star == NULL)
		return equal_nocase(pattern, pattern_len,
				    subject, subject_len);
	return wildcard_match(pattern, star - pattern,
			      star + 1, (pattern + pattern_len) - star - 1,
			      subject, subject_len);
	}

/* Compare an ASN1_STRING to a supplied string. If they match
 * return 1. If cmp_type > 0 only compare if string matches the
 * type, otherwise convert it to UTF8.
 */

static int do_check_string(ASN1_STRING *a, int cmp_type, equal_fn equal,
				const unsigned char *b, size_t blen)
	{
	if (!a->data || !a->length)
		return 0;
	if (cmp_type > 0)
		{
		if (cmp_type != a->type)
			return 0;
		if (cmp_type == V_ASN1_IA5STRING)
			return equal(a->data, a->length, b, blen);
		if (a->length == (int)blen && !memcmp(a->data, b, blen))
			return 1;
		else
			return 0;
		}
	else
		{
		int astrlen, rv;
		unsigned char *astr;
		astrlen = ASN1_STRING_to_UTF8(&astr, a);
		if (astrlen < 0)
			return -1;
		rv = equal(astr, astrlen, b, blen);
		OPENSSL_free(astr);
		return rv;
		}
	}

static int do_x509_check(X509 *x, const unsigned char *chk, size_t chklen,
					unsigned int flags, int check_type)
	{
	STACK_OF(GENERAL_NAME) *gens = NULL;
	X509_NAME *name = NULL;
	int i;
	int cnid;
	int alt_type;
	equal_fn equal;
	if (check_type == GEN_EMAIL)
		{
		cnid = NID_pkcs9_emailAddress;
		alt_type = V_ASN1_IA5STRING;
		equal = equal_email;
		}
	else if (check_type == GEN_DNS)
		{
		cnid = NID_commonName;
		alt_type = V_ASN1_IA5STRING;
		if (flags & X509_CHECK_FLAG_NO_WILDCARDS)
			equal = equal_nocase;
		else
			equal = equal_wildcard;
		}
	else
		{
		cnid = 0;
		alt_type = V_ASN1_OCTET_STRING;
		equal = equal_case;
		}

	if (chklen == 0)
		chklen = strlen((const char *)chk);

	gens = X509_get_ext_d2i(x, NID_subject_alt_name, NULL, NULL);
	if (gens)
		{
		int rv = 0;
		for (i = 0; i < sk_GENERAL_NAME_num(gens); i++)
			{
			GENERAL_NAME *gen;
			ASN1_STRING *cstr;
			gen = sk_GENERAL_NAME_value(gens, i);
			if(gen->type != check_type)
				continue;
			if (check_type == GEN_EMAIL)
				cstr = gen->d.rfc822Name;
			else if (check_type == GEN_DNS)
				cstr = gen->d.dNSName;
			else
				cstr = gen->d.iPAddress;
			if (do_check_string(cstr, alt_type, equal, chk, chklen))
				{
				rv = 1;
				break;
				}
			}
		GENERAL_NAMES_free(gens);
		if (rv)
			return 1;
		if (!(flags & X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT) || !cnid)
			return 0;
		}
	i = -1;
	name = X509_get_subject_name(x);
	while((i = X509_NAME_get_index_by_NID(name, cnid, i)) >= 0)
		{
		X509_NAME_ENTRY *ne;
		ASN1_STRING *str;
		ne = X509_NAME_get_entry(name, i);
		str = X509_NAME_ENTRY_get_data(ne);
		if (do_check_string(str, -1, equal, chk, chklen))
			return 1;
		}
	return 0;
	}

#if OPENSSL_VERSION_NUMBER < 0x1000200fL

int X509_check_host(X509 *x, const unsigned char *chk, size_t chklen,
					unsigned int flags, char **peername)
	{
	return do_x509_check(x, chk, chklen, flags, GEN_DNS);
	}

int X509_check_email(X509 *x, const unsigned char *chk, size_t chklen,
					unsigned int flags)
	{
	return do_x509_check(x, chk, chklen, flags, GEN_EMAIL);
	}

int X509_check_ip(X509 *x, const unsigned char *chk, size_t chklen,
					unsigned int flags)
	{
	return do_x509_check(x, chk, chklen, flags, GEN_IPADD);
	}

#endif /* OPENSSL_VERSION_NUMBER */

#endif
