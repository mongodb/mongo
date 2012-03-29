/*
 * Definitions used by the uuidd daemon
 *
 * Copyright (C) 2007 Theodore Ts'o.
 *
 * %Begin-Header%
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * %End-Header%
 */

#ifndef _UUID_UUIDD_H
#define _UUID_UUIDD_H

#define UUIDD_DIR		"/var/run/uuidd"
#define UUIDD_SOCKET_PATH	UUIDD_DIR "/request"
#define UUIDD_PIDFILE_PATH	UUIDD_DIR "/uuidd.pid"
#define UUIDD_PATH		"/usr/sbin/uuidd"

#define UUIDD_OP_GETPID			0
#define UUIDD_OP_GET_MAXOP		1
#define UUIDD_OP_TIME_UUID		2
#define UUIDD_OP_RANDOM_UUID		3
#define UUIDD_OP_BULK_TIME_UUID		4
#define UUIDD_OP_BULK_RANDOM_UUID	5
#define UUIDD_MAX_OP			UUIDD_OP_BULK_RANDOM_UUID

extern void uuid__generate_time(uuid_t out, int *num);
extern void uuid__generate_random(uuid_t out, int *num);

#endif /* _UUID_UUID_H */
