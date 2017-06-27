// Copyright (C) 2014 Space Monkey, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// +build windows cgo

package openssl

/*

#cgo windows LDFLAGS:  -lssleay32 -llibeay32 -L c:/openssl/bin
#cgo windows CFLAGS: -I"c:/openssl/include"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <errno.h>
#include <openssl/crypto.h>
#include <windows.h>

CRITICAL_SECTION* goopenssl_locks;

int Goopenssl_init_locks() {
	int rc = 0;
	int nlock;
	int i;
	int locks_needed = CRYPTO_num_locks();

	goopenssl_locks = (CRITICAL_SECTION*)malloc(
		sizeof(*goopenssl_locks) * locks_needed);
	if (!goopenssl_locks) {
		return ENOMEM;
	}
	for (nlock = 0; nlock < locks_needed; ++nlock) {
		InitializeCriticalSection(&goopenssl_locks[nlock]);
	}

	return 0;
}

void Goopenssl_thread_locking_callback(int mode, int n, const char *file,
	int line) {
	if (mode & CRYPTO_LOCK) {
		EnterCriticalSection(&goopenssl_locks[n]);
	} else {
		LeaveCriticalSection(&goopenssl_locks[n]);
	}
}
#if OPENSSL_VERSION_NUMBER < 0x10100000L
unsigned long Goopenssl_thread_id_callback() {
	return (unsigned long) GetCurrentThreadId();
}
#endif
*/
import "C"
