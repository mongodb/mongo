/*
 * Copyright (c) 2017-2020, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef STREAM_DEF_H_
#define STREAM_DEF_H_

#define CT_BUF_LEN 4096
#define CH_CR ('\r')
#define CH_LF ('\n')
#define CH_EQ ('=')
#define CH_DASH ('-')
#define CH_SPACE (' ')
#define CH_TAB ('\t')
#define CH_COMMA (',')
#define ST_CR ("\r")
#define ST_LF ("\n")
#define ST_CRLF ("\r\n")
#define ST_CRLFCRLF ("\r\n\r\n")
#define ST_DASHSP ("- ")
#define ST_COMMA (",")

#define ST_DASHES ("-----")
#define ST_ARMOR_BEGIN ("-----BEGIN PGP ")
#define ST_ARMOR_END ("-----END PGP ")
#define ST_CLEAR_BEGIN ("-----BEGIN PGP SIGNED MESSAGE-----")
#define ST_SIG_BEGIN ("\n-----BEGIN PGP SIGNATURE-----")
#define ST_HEADER_VERSION ("Version: ")
#define ST_HEADER_COMMENT ("Comment: ")
#define ST_HEADER_HASH ("Hash: ")
#define ST_HEADER_CHARSET ("Charset: ")
#define ST_FROM ("From")

/* Preallocated cache length for AEAD encryption/decryption */
#define PGP_AEAD_CACHE_LEN (PGP_INPUT_CACHE_SIZE + 2 * PGP_AEAD_MAX_TAG_LEN)

/* Maximum OpenPGP packet nesting level */
#define MAXIMUM_NESTING_LEVEL 32
#define MAXIMUM_STREAM_PKTS 16
#define MAXIMUM_ERROR_PKTS 64

/* Maximum text line length supported by GnuPG */
#define MAXIMUM_GNUPG_LINELEN 19995

#endif /* !STREAM_DEF_H_ */
