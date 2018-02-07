// Copyright (C) 2017. See AUTHORS.
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

// +build linux darwin cgo
// +build !windows

package openssl

/*
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#include <errno.h>
#include <openssl/crypto.h>
#include <pthread.h>

pthread_mutex_t* goopenssl_locks;

int go_init_locks() {
	int rc = 0;
	int nlock;
	int i;
	int locks_needed = CRYPTO_num_locks();

	goopenssl_locks = (pthread_mutex_t*)malloc(
		sizeof(pthread_mutex_t) * locks_needed);
	if (!goopenssl_locks) {
		return ENOMEM;
	}
	for (nlock = 0; nlock < locks_needed; ++nlock) {
		rc = pthread_mutex_init(&goopenssl_locks[nlock], NULL);
		if (rc != 0) {
			break;
		}
	}

	if (rc != 0) {
		for (i = nlock - 1; i >= 0; --i) {
			pthread_mutex_destroy(&goopenssl_locks[i]);
		}
		free(goopenssl_locks);
		goopenssl_locks = NULL;
	}
	return rc;
}

void go_thread_locking_callback(int mode, int n, const char *file,
	int line) {
	if (mode & CRYPTO_LOCK) {
		pthread_mutex_lock(&goopenssl_locks[n]);
	} else {
		pthread_mutex_unlock(&goopenssl_locks[n]);
	}
}

unsigned long go_thread_id_callback() {
	return (unsigned long) pthread_self();
}
#endif
*/
import "C"
