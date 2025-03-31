/*
 * Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <bson/bson-compat.h>

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <bson/bson-atomic.h>
#include <bson/bson-clock.h>
#include <bson/bson-context.h>
#include <bson/bson-context-private.h>
#include <bson/bson-memory.h>
#include "common-thread-private.h"


#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif


/*
 * Globals.
 */
static bson_context_t gContextDefault;

static BSON_INLINE uint64_t
_bson_getpid (void)
{
   uint64_t pid;
#ifdef BSON_OS_WIN32
   DWORD real_pid;

   real_pid = GetCurrentProcessId ();
   pid = (real_pid & 0xFFFF) ^ ((real_pid >> 16) & 0xFFFF);
#else
   pid = (uint64_t) getpid ();
#endif

   return pid;
}


void
_bson_context_set_oid_seq32 (bson_context_t *context, /* IN */
                             bson_oid_t *oid)         /* OUT */
{
   uint32_t seq = (uint32_t) bson_atomic_int32_fetch_add (
      (DECL_ATOMIC_INTEGRAL_INT32 *) &context->seq32, 1, bson_memory_order_seq_cst);
   seq = BSON_UINT32_TO_BE (seq);
   memcpy (&oid->bytes[BSON_OID_SEQ32_OFFSET], ((uint8_t *) &seq) + 1, BSON_OID_SEQ32_SIZE);
}


void
_bson_context_set_oid_seq64 (bson_context_t *context, /* IN */
                             bson_oid_t *oid)         /* OUT */
{
   uint64_t seq = (uint64_t) bson_atomic_int64_fetch_add ((int64_t *) &context->seq64, 1, bson_memory_order_seq_cst);

   seq = BSON_UINT64_TO_BE (seq);
   memcpy (&oid->bytes[BSON_OID_SEQ64_OFFSET], &seq, BSON_OID_SEQ64_SIZE);
}

/*
 * --------------------------------------------------------------------------
 *
 * _bson_context_get_hostname
 *
 *       Gets the hostname of the machine, logs a warning on failure. "out"
 *       must be an array of HOST_NAME_MAX bytes.
 *
 * --------------------------------------------------------------------------
 */
static void
_bson_context_get_hostname (char out[HOST_NAME_MAX])
{
   if (gethostname (out, HOST_NAME_MAX) != 0) {
      if (errno == ENAMETOOLONG) {
         fprintf (stderr, "hostname exceeds %d characters, truncating.", HOST_NAME_MAX);
      } else {
         fprintf (stderr, "unable to get hostname: %d", errno);
      }
   }
   out[HOST_NAME_MAX - 1] = '\0';
}


/*** ========================================
 * The below SipHash implementation is based on the original public-domain
 * reference implementation from Jean-Philippe Aumasson and DJB
 * (https://github.com/veorq/SipHash).
 */

/* in-place rotate a 64bit number */
void
_bson_rotl_u64 (uint64_t *p, int nbits)
{
   *p = (*p << nbits) | (*p >> (64 - nbits));
}

/* Write the little-endian representation of 'val' into 'out' */
void
_u64_into_u8x8_le (uint8_t out[8], uint64_t val)
{
   val = BSON_UINT64_TO_LE (val);
   memcpy (out, &val, sizeof val);
}

/* Read a little-endian representation of a 64bit number from 'in' */
uint64_t
_u8x8_le_to_u64 (const uint8_t in[8])
{
   uint64_t r;
   memcpy (&r, in, sizeof r);
   return BSON_UINT64_FROM_LE (r);
}

/* Perform one SipHash round */
void
_sip_round (uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3)
{
   *v0 += *v1;
   _bson_rotl_u64 (v1, 13);
   *v1 ^= *v0;
   _bson_rotl_u64 (v0, 32);
   *v2 += *v3;
   _bson_rotl_u64 (v3, 16);
   *v3 ^= *v2;
   *v0 += *v3;
   _bson_rotl_u64 (v3, 21);
   *v3 ^= *v0;
   *v2 += *v1;
   _bson_rotl_u64 (v1, 17);
   *v1 ^= *v2;
   _bson_rotl_u64 (v2, 32);
}

void
_siphash (const void *in, const size_t inlen, const uint64_t key[2], uint64_t digest[2])
{
   const unsigned char *ni = (const unsigned char *) in;
   const unsigned char *kk = (const unsigned char *) key;
   uint8_t digest_buf[16] = {0};

   const int C_ROUNDS = 2;
   const int D_ROUNDS = 4;

   uint64_t v0 = UINT64_C (0x736f6d6570736575);
   uint64_t v1 = UINT64_C (0x646f72616e646f6d);
   uint64_t v2 = UINT64_C (0x6c7967656e657261);
   uint64_t v3 = UINT64_C (0x7465646279746573);
   uint64_t k0 = _u8x8_le_to_u64 (kk);
   uint64_t k1 = _u8x8_le_to_u64 (kk + 8);
   uint64_t m;
   int i;
   const unsigned char *end = ni + inlen - (inlen % sizeof (uint64_t));
   const int left = inlen & 7;
   uint64_t b = ((uint64_t) inlen) << 56;
   v3 ^= k1;
   v2 ^= k0;
   v1 ^= k1;
   v0 ^= k0;

   v1 ^= 0xee;

   for (; ni != end; ni += 8) {
      m = _u8x8_le_to_u64 (ni);
      v3 ^= m;

      for (i = 0; i < C_ROUNDS; ++i)
         _sip_round (&v0, &v1, &v2, &v3);

      v0 ^= m;
   }

   switch (left) {
   case 7:
      b |= ((uint64_t) ni[6]) << 48;
      /* FALLTHRU */
   case 6:
      b |= ((uint64_t) ni[5]) << 40;
      /* FALLTHRU */
   case 5:
      b |= ((uint64_t) ni[4]) << 32;
      /* FALLTHRU */
   case 4:
      b |= ((uint64_t) ni[3]) << 24;
      /* FALLTHRU */
   case 3:
      b |= ((uint64_t) ni[2]) << 16;
      /* FALLTHRU */
   case 2:
      b |= ((uint64_t) ni[1]) << 8;
      /* FALLTHRU */
   case 1:
      b |= ((uint64_t) ni[0]);
      break;
   default:
      BSON_UNREACHABLE ("Invalid remainder during SipHash");
   case 0:
      break;
   }

   v3 ^= b;

   for (i = 0; i < C_ROUNDS; ++i)
      _sip_round (&v0, &v1, &v2, &v3);

   v0 ^= b;

   v2 ^= 0xee;

   for (i = 0; i < D_ROUNDS; ++i)
      _sip_round (&v0, &v1, &v2, &v3);

   b = v0 ^ v1 ^ v2 ^ v3;
   _u64_into_u8x8_le (digest_buf, b);

   v1 ^= 0xdd;

   for (i = 0; i < D_ROUNDS; ++i)
      _sip_round (&v0, &v1, &v2, &v3);

   b = v0 ^ v1 ^ v2 ^ v3;
   _u64_into_u8x8_le (digest_buf + 8, b);

   memcpy (digest, digest_buf, sizeof digest_buf);
}

/*
 * The seed consists of the following hashed together:
 * - current time (with microsecond resolution)
 * - current pid
 * - current hostname
 * - The init-call counter
 */
struct _init_rand_params {
   struct timeval time;
   uint64_t pid;
   char hostname[HOST_NAME_MAX];
   int64_t rand_call_counter;
};

static void
_bson_context_init_random (bson_context_t *context, bool init_seq)
{
   /* Keep an atomic counter of this function being called. This is used to add
    * additional input to the random hash, ensuring no two calls in a single
    * process will receive identical hash inputs, even occurring at the same
    * microsecond. */
   static int64_t s_rand_call_counter = INT64_MIN;

   /* The message digest of the random params */
   uint64_t digest[2] = {0};
   uint64_t key[2] = {0};
   /* The randomness parameters */
   struct _init_rand_params rand_params;

   /* Init each part of the randomness source: */
   memset (&rand_params, 0, sizeof rand_params);
   bson_gettimeofday (&rand_params.time);
   rand_params.pid = _bson_getpid ();
   _bson_context_get_hostname (rand_params.hostname);
   rand_params.rand_call_counter = bson_atomic_int64_fetch_add (&s_rand_call_counter, 1, bson_memory_order_seq_cst);

   /* Generate a SipHash key. We do not care about secrecy or determinism, only
    * uniqueness. */
   memcpy (key, &rand_params, sizeof key);
   key[1] = ~key[0];

   /* Hash the param struct */
   _siphash (&rand_params, sizeof rand_params, key, digest);

   /** Initialize the rand and sequence counters with our random digest */
   memcpy (context->randomness, digest, sizeof context->randomness);
   if (init_seq) {
      memcpy (&context->seq32, digest + 1, sizeof context->seq32);
      memcpy (&context->seq64, digest + 1, sizeof context->seq64);
      /* Chop off some initial bits for nicer counter behavior. This allows the
       * low digit to start at a zero, and prevents immediately wrapping the
       * counter in subsequent calls to set_oid_seq. */
      context->seq32 &= ~UINT32_C (0xf0000f);
      context->seq64 &= ~UINT64_C (0xf0000f);
   }

   /* Remember the PID we saw here. This may change in case of fork() */
   context->pid = rand_params.pid;
}

static void
_bson_context_init (bson_context_t *context, bson_context_flags_t flags)
{
   context->flags = (int) flags;
   _bson_context_init_random (context, true /* Init counters */);
}


void
_bson_context_set_oid_rand (bson_context_t *context, bson_oid_t *oid)
{
   BSON_ASSERT (context);
   BSON_ASSERT (oid);

   if (context->flags & BSON_CONTEXT_DISABLE_PID_CACHE) {
      /* User has requested that we check if our PID has changed. This can occur
       * after a call to fork() */
      uint64_t now_pid = _bson_getpid ();
      if (now_pid != context->pid) {
         _bson_context_init_random (context, false /* Do not update the sequence counters */);
      }
   }
   /* Copy the stored randomness into the OID */
   memcpy (oid->bytes + BSON_OID_RANDOMESS_OFFSET, &context->randomness, BSON_OID_RANDOMNESS_SIZE);
}


bson_context_t *
bson_context_new (bson_context_flags_t flags)
{
   bson_context_t *context;

   context = bson_malloc0 (sizeof *context);
   _bson_context_init (context, flags);

   return context;
}


void
bson_context_destroy (bson_context_t *context) /* IN */
{
   bson_free (context);
}


static BSON_ONCE_FUN (_bson_context_init_default)
{
   _bson_context_init (&gContextDefault, BSON_CONTEXT_DISABLE_PID_CACHE);
   BSON_ONCE_RETURN;
}


bson_context_t *
bson_context_get_default (void)
{
   static bson_once_t once = BSON_ONCE_INIT;

   bson_once (&once, _bson_context_init_default);

   return &gContextDefault;
}
