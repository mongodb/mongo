/*    Copyright 2014 MongoDB Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/* ---- LTC_BASE64 Routines ---- */
#ifdef LTC_BASE64
int base64_encode(const unsigned char* in,
                  unsigned long len,
                  unsigned char* out,
                  unsigned long* outlen);

int base64_decode(const unsigned char* in,
                  unsigned long len,
                  unsigned char* out,
                  unsigned long* outlen);
#endif

/* ---- MEM routines ---- */
void zeromem(void* dst, size_t len);
void burn_stack(unsigned long len);

const char* error_to_string(int err);

extern const char* crypt_build_settings;

/* ---- HMM ---- */
int crypt_fsa(void* mp, ...);

/* $Source: /cvs/libtom/libtomcrypt/src/headers/tomcrypt_misc.h,v $ */
/* $Revision: 1.5 $ */
/* $Date: 2007/05/12 14:32:35 $ */
