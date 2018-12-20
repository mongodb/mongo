/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

#ifndef TOMCRYPT_CUSTOM_H_
#define TOMCRYPT_CUSTOM_H_

/* macros for various libc functions you can change for embedded targets */
#ifndef XMALLOC
#define XMALLOC  malloc
#endif
#ifndef XREALLOC
#define XREALLOC realloc
#endif
#ifndef XCALLOC
#define XCALLOC  calloc
#endif
#ifndef XFREE
#define XFREE    free
#endif

#ifndef XMEMSET
#define XMEMSET  memset
#endif
#ifndef XMEMCPY
#define XMEMCPY  memcpy
#endif
#ifndef XMEMMOVE
#define XMEMMOVE memmove
#endif
#ifndef XMEMCMP
#define XMEMCMP  memcmp
#endif
/* A memory compare function that has to run in constant time,
 * c.f. mem_neq() API summary.
 */
#ifndef XMEM_NEQ
#define XMEM_NEQ  mem_neq
#endif
#ifndef XSTRCMP
#define XSTRCMP strcmp
#endif

#ifndef XCLOCK
#define XCLOCK   clock
#endif

#ifndef XQSORT
#define XQSORT qsort
#endif

#if ( defined(malloc) || defined(realloc) || defined(calloc) || defined(free) || \
      defined(memset) || defined(memcpy) || defined(memcmp) || defined(strcmp) || \
      defined(clock) || defined(qsort) ) && !defined(LTC_NO_PROTOTYPES)
#define LTC_NO_PROTOTYPES
#endif

/* shortcut to disable automatic inclusion */
#if defined LTC_NOTHING && !defined LTC_EASY
  #define LTC_NO_CIPHERS
  #define LTC_NO_MODES
  #define LTC_NO_HASHES
  #define LTC_NO_MACS
  #define LTC_NO_PRNGS
  #define LTC_NO_PK
  #define LTC_NO_PKCS
  #define LTC_NO_MISC
#endif /* LTC_NOTHING */

/* Easy button? */
#ifdef LTC_EASY
   #define LTC_NO_CIPHERS
   #define LTC_RIJNDAEL
   #define LTC_BLOWFISH
   #define LTC_DES
   #define LTC_CAST5

   #define LTC_NO_MODES
   #define LTC_ECB_MODE
   #define LTC_CBC_MODE
   #define LTC_CTR_MODE

   #define LTC_NO_HASHES
   #define LTC_SHA1
   #define LTC_SHA3
   #define LTC_SHA512
   #define LTC_SHA384
   #define LTC_SHA256
   #define LTC_SHA224
   #define LTC_HASH_HELPERS

   #define LTC_NO_MACS
   #define LTC_HMAC
   #define LTC_OMAC
   #define LTC_CCM_MODE

   #define LTC_NO_PRNGS
   #define LTC_SPRNG
   #define LTC_YARROW
   #define LTC_DEVRANDOM
   #define LTC_TRY_URANDOM_FIRST
   #define LTC_RNG_GET_BYTES
   #define LTC_RNG_MAKE_PRNG

   #define LTC_NO_PK
   #define LTC_MRSA
   #define LTC_MECC

   #define LTC_NO_MISC
   #define LTC_BASE64
#endif

/* The minimal set of functionality to run the tests */
#ifdef LTC_MINIMAL
   #define LTC_RIJNDAEL
   #define LTC_SHA256
   #define LTC_YARROW
   #define LTC_CTR_MODE

   #define LTC_RNG_MAKE_PRNG
   #define LTC_RNG_GET_BYTES
   #define LTC_DEVRANDOM
   #define LTC_TRY_URANDOM_FIRST

   #undef LTC_NO_FILE
#endif

/* Enable self-test test vector checking */
#ifndef LTC_NO_TEST
   #define LTC_TEST
#endif
/* Enable extended self-tests */
/* #define LTC_TEST_EXT */

/* Use small code where possible */
/* #define LTC_SMALL_CODE */

/* clean the stack of functions which put private information on stack */
/* #define LTC_CLEAN_STACK */

/* disable all file related functions */
/* #define LTC_NO_FILE */

/* disable all forms of ASM */
/* #define LTC_NO_ASM */

/* disable FAST mode */
/* #define LTC_NO_FAST */

/* disable BSWAP on x86 */
/* #define LTC_NO_BSWAP */

/* ---> math provider? <--- */
#ifndef LTC_NO_MATH

/* LibTomMath */
/* #define LTM_DESC */

/* TomsFastMath */
/* #define TFM_DESC */

/* GNU Multiple Precision Arithmetic Library */
/* #define GMP_DESC */

#endif /* LTC_NO_MATH */

/* ---> Symmetric Block Ciphers <--- */
#ifndef LTC_NO_CIPHERS

#define LTC_BLOWFISH
#define LTC_RC2
#define LTC_RC5
#define LTC_RC6
#define LTC_SAFERP
#define LTC_RIJNDAEL
#define LTC_XTEA
/* _TABLES tells it to use tables during setup, _SMALL means to use the smaller scheduled key format
 * (saves 4KB of ram), _ALL_TABLES enables all tables during setup */
#define LTC_TWOFISH
#ifndef LTC_NO_TABLES
   #define LTC_TWOFISH_TABLES
   /* #define LTC_TWOFISH_ALL_TABLES */
#else
   #define LTC_TWOFISH_SMALL
#endif
/* #define LTC_TWOFISH_SMALL */
/* LTC_DES includes EDE triple-DES */
#define LTC_DES
#define LTC_CAST5
#define LTC_NOEKEON
#define LTC_SKIPJACK
#define LTC_SAFER
#define LTC_KHAZAD
#define LTC_ANUBIS
#define LTC_ANUBIS_TWEAK
#define LTC_KSEED
#define LTC_KASUMI
#define LTC_MULTI2
#define LTC_CAMELLIA

/* stream ciphers */
#define LTC_CHACHA
#define LTC_RC4_STREAM
#define LTC_SOBER128_STREAM

#endif /* LTC_NO_CIPHERS */


/* ---> Block Cipher Modes of Operation <--- */
#ifndef LTC_NO_MODES

#define LTC_CFB_MODE
#define LTC_OFB_MODE
#define LTC_ECB_MODE
#define LTC_CBC_MODE
#define LTC_CTR_MODE

/* F8 chaining mode */
#define LTC_F8_MODE

/* LRW mode */
#define LTC_LRW_MODE
#ifndef LTC_NO_TABLES
   /* like GCM mode this will enable 16 8x128 tables [64KB] that make
    * seeking very fast.
    */
   #define LTC_LRW_TABLES
#endif

/* XTS mode */
#define LTC_XTS_MODE

#endif /* LTC_NO_MODES */

/* ---> One-Way Hash Functions <--- */
#ifndef LTC_NO_HASHES

#define LTC_CHC_HASH
#define LTC_WHIRLPOOL
#define LTC_SHA3
#define LTC_SHA512
#define LTC_SHA512_256
#define LTC_SHA512_224
#define LTC_SHA384
#define LTC_SHA256
#define LTC_SHA224
#define LTC_TIGER
#define LTC_SHA1
#define LTC_MD5
#define LTC_MD4
#define LTC_MD2
#define LTC_RIPEMD128
#define LTC_RIPEMD160
#define LTC_RIPEMD256
#define LTC_RIPEMD320
#define LTC_BLAKE2S
#define LTC_BLAKE2B

#define LTC_HASH_HELPERS

#endif /* LTC_NO_HASHES */


/* ---> MAC functions <--- */
#ifndef LTC_NO_MACS

#define LTC_HMAC
#define LTC_OMAC
#define LTC_PMAC
#define LTC_XCBC
#define LTC_F9_MODE
#define LTC_PELICAN
#define LTC_POLY1305
#define LTC_BLAKE2SMAC
#define LTC_BLAKE2BMAC

/* ---> Encrypt + Authenticate Modes <--- */

#define LTC_EAX_MODE

#define LTC_OCB_MODE
#define LTC_OCB3_MODE
#define LTC_CCM_MODE
#define LTC_GCM_MODE
#define LTC_CHACHA20POLY1305_MODE

/* Use 64KiB tables */
#ifndef LTC_NO_TABLES
   #define LTC_GCM_TABLES
#endif

/* USE SSE2? requires GCC works on x86_32 and x86_64*/
#ifdef LTC_GCM_TABLES
/* #define LTC_GCM_TABLES_SSE2 */
#endif

#endif /* LTC_NO_MACS */


/* --> Pseudo Random Number Generators <--- */
#ifndef LTC_NO_PRNGS

/* Yarrow */
#define LTC_YARROW

/* a PRNG that simply reads from an available system source */
#define LTC_SPRNG

/* The RC4 stream cipher based PRNG */
#define LTC_RC4

/* The ChaCha20 stream cipher based PRNG */
#define LTC_CHACHA20_PRNG

/* Fortuna PRNG */
#define LTC_FORTUNA

/* Greg's SOBER128 stream cipher based PRNG */
#define LTC_SOBER128

/* the *nix style /dev/random device */
#define LTC_DEVRANDOM
/* try /dev/urandom before trying /dev/random
 * are you sure you want to disable this? http://www.2uo.de/myths-about-urandom/ */
#define LTC_TRY_URANDOM_FIRST
/* rng_get_bytes() */
#define LTC_RNG_GET_BYTES
/* rng_make_prng() */
#define LTC_RNG_MAKE_PRNG

/* enable the ltc_rng hook to integrate e.g. embedded hardware RNG's easily */
/* #define LTC_PRNG_ENABLE_LTC_RNG */

#endif /* LTC_NO_PRNGS */

#ifdef LTC_YARROW

/* which descriptor of AES to use?  */
/* 0 = rijndael_enc 1 = aes_enc, 2 = rijndael [full], 3 = aes [full] */
#ifdef ENCRYPT_ONLY
  #define LTC_YARROW_AES 0
#else
  #define LTC_YARROW_AES 2
#endif

#endif

#ifdef LTC_FORTUNA

#ifndef LTC_FORTUNA_WD
/* reseed every N calls to the read function */
#define LTC_FORTUNA_WD    10
#endif

#ifndef LTC_FORTUNA_POOLS
/* number of pools (4..32) can save a bit of ram by lowering the count */
#define LTC_FORTUNA_POOLS 32
#endif

#endif /* LTC_FORTUNA */


/* ---> Public Key Crypto <--- */
#ifndef LTC_NO_PK

/* Include RSA support */
#define LTC_MRSA

/* Include Diffie-Hellman support */
/* is_prime fails for GMP */
#define LTC_MDH
/* Supported Key Sizes */
#define LTC_DH768
#define LTC_DH1024
#define LTC_DH1536
#define LTC_DH2048

#ifndef TFM_DESC
/* tfm has a problem in fp_isprime for larger key sizes */
#define LTC_DH3072
#define LTC_DH4096
#define LTC_DH6144
#define LTC_DH8192
#endif

/* Include Katja (a Rabin variant like RSA) */
/* #define LTC_MKAT */

/* Digital Signature Algorithm */
#define LTC_MDSA

/* ECC */
#define LTC_MECC

/* use Shamir's trick for point mul (speeds up signature verification) */
#define LTC_ECC_SHAMIR

#if defined(TFM_DESC) && defined(LTC_MECC)
   #define LTC_MECC_ACCEL
#endif

/* do we want fixed point ECC */
/* #define LTC_MECC_FP */

#endif /* LTC_NO_PK */

#if defined(LTC_MRSA) && !defined(LTC_NO_RSA_BLINDING)
/* Enable RSA blinding when doing private key operations by default */
#define LTC_RSA_BLINDING
#endif  /* LTC_NO_RSA_BLINDING */

#if defined(LTC_MRSA) && !defined(LTC_NO_RSA_CRT_HARDENING)
/* Enable RSA CRT hardening when doing private key operations by default */
#define LTC_RSA_CRT_HARDENING
#endif  /* LTC_NO_RSA_CRT_HARDENING */

#if defined(LTC_MECC) && !defined(LTC_NO_ECC_TIMING_RESISTANT)
/* Enable ECC timing resistant version by default */
#define LTC_ECC_TIMING_RESISTANT
#endif

/* PKCS #1 (RSA) and #5 (Password Handling) stuff */
#ifndef LTC_NO_PKCS

#define LTC_PKCS_1
#define LTC_PKCS_5

/* Include ASN.1 DER (required by DSA/RSA) */
#define LTC_DER

#endif /* LTC_NO_PKCS */

/* misc stuff */
#ifndef LTC_NO_MISC

/* Various tidbits of modern neatoness */
#define LTC_BASE64
/* ... and it's URL safe version */
#define LTC_BASE64_URL

/* Keep LTC_NO_HKDF for compatibility reasons
 * superseeded by LTC_NO_MISC*/
#ifndef LTC_NO_HKDF
/* HKDF Key Derivation/Expansion stuff */
#define LTC_HKDF
#endif /* LTC_NO_HKDF */

#define LTC_ADLER32

#define LTC_CRC32

#endif /* LTC_NO_MISC */

/* cleanup */

#ifdef LTC_MECC
/* Supported ECC Key Sizes */
#ifndef LTC_NO_CURVES
   #define LTC_ECC112
   #define LTC_ECC128
   #define LTC_ECC160
   #define LTC_ECC192
   #define LTC_ECC224
   #define LTC_ECC256
   #define LTC_ECC384
   #define LTC_ECC521
#endif
#endif

#if defined(LTC_DER)
   #ifndef LTC_DER_MAX_RECURSION
      /* Maximum recursion limit when processing nested ASN.1 types. */
      #define LTC_DER_MAX_RECURSION 30
   #endif
#endif

#if defined(LTC_MECC) || defined(LTC_MRSA) || defined(LTC_MDSA) || defined(LTC_MKAT)
   /* Include the MPI functionality?  (required by the PK algorithms) */
   #define LTC_MPI

   #ifndef LTC_PK_MAX_RETRIES
      /* iterations limit for retry-loops */
      #define LTC_PK_MAX_RETRIES  20
   #endif
#endif

#ifdef LTC_MRSA
   #define LTC_PKCS_1
#endif

#if defined(LTC_PELICAN) && !defined(LTC_RIJNDAEL)
   #error Pelican-MAC requires LTC_RIJNDAEL
#endif

#if defined(LTC_EAX_MODE) && !(defined(LTC_CTR_MODE) && defined(LTC_OMAC))
   #error LTC_EAX_MODE requires CTR and LTC_OMAC mode
#endif

#if defined(LTC_YARROW) && !defined(LTC_CTR_MODE)
   #error LTC_YARROW requires LTC_CTR_MODE chaining mode to be defined!
#endif

#if defined(LTC_DER) && !defined(LTC_MPI)
   #error ASN.1 DER requires MPI functionality
#endif

#if (defined(LTC_MDSA) || defined(LTC_MRSA) || defined(LTC_MECC) || defined(LTC_MKAT)) && !defined(LTC_DER)
   #error PK requires ASN.1 DER functionality, make sure LTC_DER is enabled
#endif

#if defined(LTC_CHACHA20POLY1305_MODE) && (!defined(LTC_CHACHA) || !defined(LTC_POLY1305))
   #error LTC_CHACHA20POLY1305_MODE requires LTC_CHACHA + LTC_POLY1305
#endif

#if defined(LTC_CHACHA20_PRNG) && !defined(LTC_CHACHA)
   #error LTC_CHACHA20_PRNG requires LTC_CHACHA
#endif

#if defined(LTC_RC4) && !defined(LTC_RC4_STREAM)
   #error LTC_RC4 requires LTC_RC4_STREAM
#endif

#if defined(LTC_SOBER128) && !defined(LTC_SOBER128_STREAM)
   #error LTC_SOBER128 requires LTC_SOBER128_STREAM
#endif

#if defined(LTC_BLAKE2SMAC) && !defined(LTC_BLAKE2S)
   #error LTC_BLAKE2SMAC requires LTC_BLAKE2S
#endif

#if defined(LTC_BLAKE2BMAC) && !defined(LTC_BLAKE2B)
   #error LTC_BLAKE2BMAC requires LTC_BLAKE2B
#endif

#if defined(LTC_SPRNG) && !defined(LTC_RNG_GET_BYTES)
   #error LTC_SPRNG requires LTC_RNG_GET_BYTES
#endif

#if defined(LTC_NO_MATH) && (defined(LTM_DESC) || defined(TFM_DESC) || defined(GMP_DESC))
   #error LTC_NO_MATH defined, but also a math descriptor
#endif

/* THREAD management */
#ifdef LTC_PTHREAD

#include <pthread.h>

#define LTC_MUTEX_GLOBAL(x)   pthread_mutex_t x = PTHREAD_MUTEX_INITIALIZER;
#define LTC_MUTEX_PROTO(x)    extern pthread_mutex_t x;
#define LTC_MUTEX_TYPE(x)     pthread_mutex_t x;
#define LTC_MUTEX_INIT(x)     LTC_ARGCHK(pthread_mutex_init(x, NULL) == 0);
#define LTC_MUTEX_LOCK(x)     LTC_ARGCHK(pthread_mutex_lock(x) == 0);
#define LTC_MUTEX_UNLOCK(x)   LTC_ARGCHK(pthread_mutex_unlock(x) == 0);
#define LTC_MUTEX_DESTROY(x)  LTC_ARGCHK(pthread_mutex_destroy(x) == 0);

#else

/* default no functions */
#define LTC_MUTEX_GLOBAL(x)
#define LTC_MUTEX_PROTO(x)
#define LTC_MUTEX_TYPE(x)
#define LTC_MUTEX_INIT(x)
#define LTC_MUTEX_LOCK(x)
#define LTC_MUTEX_UNLOCK(x)
#define LTC_MUTEX_DESTROY(x)

#endif

/* Debuggers */

/* define this if you use Valgrind, note: it CHANGES the way SOBER-128 and RC4 work (see the code) */
/* #define LTC_VALGRIND */

#endif

#ifndef LTC_NO_FILE
   /* buffer size for reading from a file via fread(..) */
   #ifndef LTC_FILE_READ_BUFSIZE
   #define LTC_FILE_READ_BUFSIZE 8192
   #endif
#endif

/* ref:         HEAD -> master, tag: v1.18.2 */
/* git commit:  7e7eb695d581782f04b24dc444cbfde86af59853 */
/* commit time: 2018-07-01 22:49:01 +0200 */
