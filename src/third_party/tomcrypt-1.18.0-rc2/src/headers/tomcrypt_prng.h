/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

/* ---- PRNG Stuff ---- */
#ifdef LTC_YARROW
struct yarrow_prng {
    int                   cipher, hash;
    unsigned char         pool[MAXBLOCKSIZE];
    symmetric_CTR         ctr;
};
#endif

#ifdef LTC_RC4
struct rc4_prng {
    rc4_state s;
};
#endif

#ifdef LTC_CHACHA20_PRNG
struct chacha20_prng {
    chacha_state s;        /* chacha state */
    unsigned char ent[40]; /* entropy buffer */
    unsigned long idx;     /* entropy counter */
};
#endif

#ifdef LTC_FORTUNA
struct fortuna_prng {
    hash_state pool[LTC_FORTUNA_POOLS];     /* the  pools */

    symmetric_key skey;

    unsigned char K[32],      /* the current key */
                  IV[16];     /* IV for CTR mode */

    unsigned long pool_idx,   /* current pool we will add to */
                  pool0_len,  /* length of 0'th pool */
                  wd;

    ulong64       reset_cnt;  /* number of times we have reset */
};
#endif

#ifdef LTC_SOBER128
struct sober128_prng {
    sober128_state s;      /* sober128 state */
    unsigned char ent[40]; /* entropy buffer */
    unsigned long idx;     /* entropy counter */
};
#endif

typedef struct {
   union {
      char dummy[1];
#ifdef LTC_YARROW
      struct yarrow_prng    yarrow;
#endif
#ifdef LTC_RC4
      struct rc4_prng       rc4;
#endif
#ifdef LTC_CHACHA20_PRNG
      struct chacha20_prng  chacha;
#endif
#ifdef LTC_FORTUNA
      struct fortuna_prng   fortuna;
#endif
#ifdef LTC_SOBER128
      struct sober128_prng  sober128;
#endif
   };
   short ready;            /* ready flag 0-1 */
   LTC_MUTEX_TYPE(lock)    /* lock */
} prng_state;

/** PRNG descriptor */
extern struct ltc_prng_descriptor {
    /** Name of the PRNG */
    char *name;
    /** size in bytes of exported state */
    int  export_size;
    /** Start a PRNG state
        @param prng   [out] The state to initialize
        @return CRYPT_OK if successful
    */
    int (*start)(prng_state *prng);
    /** Add entropy to the PRNG
        @param in         The entropy
        @param inlen      Length of the entropy (octets)\
        @param prng       The PRNG state
        @return CRYPT_OK if successful
    */
    int (*add_entropy)(const unsigned char *in, unsigned long inlen, prng_state *prng);
    /** Ready a PRNG state to read from
        @param prng       The PRNG state to ready
        @return CRYPT_OK if successful
    */
    int (*ready)(prng_state *prng);
    /** Read from the PRNG
        @param out     [out] Where to store the data
        @param outlen  Length of data desired (octets)
        @param prng    The PRNG state to read from
        @return Number of octets read
    */
    unsigned long (*read)(unsigned char *out, unsigned long outlen, prng_state *prng);
    /** Terminate a PRNG state
        @param prng   The PRNG state to terminate
        @return CRYPT_OK if successful
    */
    int (*done)(prng_state *prng);
    /** Export a PRNG state
        @param out     [out] The destination for the state
        @param outlen  [in/out] The max size and resulting size of the PRNG state
        @param prng    The PRNG to export
        @return CRYPT_OK if successful
    */
    int (*pexport)(unsigned char *out, unsigned long *outlen, prng_state *prng);
    /** Import a PRNG state
        @param in      The data to import
        @param inlen   The length of the data to import (octets)
        @param prng    The PRNG to initialize/import
        @return CRYPT_OK if successful
    */
    int (*pimport)(const unsigned char *in, unsigned long inlen, prng_state *prng);
    /** Self-test the PRNG
        @return CRYPT_OK if successful, CRYPT_NOP if self-testing has been disabled
    */
    int (*test)(void);
} prng_descriptor[];

#ifdef LTC_YARROW
int yarrow_start(prng_state *prng);
int yarrow_add_entropy(const unsigned char *in, unsigned long inlen, prng_state *prng);
int yarrow_ready(prng_state *prng);
unsigned long yarrow_read(unsigned char *out, unsigned long outlen, prng_state *prng);
int yarrow_done(prng_state *prng);
int  yarrow_export(unsigned char *out, unsigned long *outlen, prng_state *prng);
int  yarrow_import(const unsigned char *in, unsigned long inlen, prng_state *prng);
int  yarrow_test(void);
extern const struct ltc_prng_descriptor yarrow_desc;
#endif

#ifdef LTC_FORTUNA
int fortuna_start(prng_state *prng);
int fortuna_add_entropy(const unsigned char *in, unsigned long inlen, prng_state *prng);
int fortuna_ready(prng_state *prng);
unsigned long fortuna_read(unsigned char *out, unsigned long outlen, prng_state *prng);
int fortuna_done(prng_state *prng);
int  fortuna_export(unsigned char *out, unsigned long *outlen, prng_state *prng);
int  fortuna_import(const unsigned char *in, unsigned long inlen, prng_state *prng);
int  fortuna_test(void);
extern const struct ltc_prng_descriptor fortuna_desc;
#endif

#ifdef LTC_RC4
int rc4_start(prng_state *prng);
int rc4_add_entropy(const unsigned char *in, unsigned long inlen, prng_state *prng);
int rc4_ready(prng_state *prng);
unsigned long rc4_read(unsigned char *out, unsigned long outlen, prng_state *prng);
int  rc4_done(prng_state *prng);
int  rc4_export(unsigned char *out, unsigned long *outlen, prng_state *prng);
int  rc4_import(const unsigned char *in, unsigned long inlen, prng_state *prng);
int  rc4_test(void);
extern const struct ltc_prng_descriptor rc4_desc;
#endif

#ifdef LTC_CHACHA20_PRNG
int chacha20_prng_start(prng_state *prng);
int chacha20_prng_add_entropy(const unsigned char *in, unsigned long inlen, prng_state *prng);
int chacha20_prng_ready(prng_state *prng);
unsigned long chacha20_prng_read(unsigned char *out, unsigned long outlen, prng_state *prng);
int  chacha20_prng_done(prng_state *prng);
int  chacha20_prng_export(unsigned char *out, unsigned long *outlen, prng_state *prng);
int  chacha20_prng_import(const unsigned char *in, unsigned long inlen, prng_state *prng);
int  chacha20_prng_test(void);
extern const struct ltc_prng_descriptor chacha20_prng_desc;
#endif

#ifdef LTC_SPRNG
int sprng_start(prng_state *prng);
int sprng_add_entropy(const unsigned char *in, unsigned long inlen, prng_state *prng);
int sprng_ready(prng_state *prng);
unsigned long sprng_read(unsigned char *out, unsigned long outlen, prng_state *prng);
int sprng_done(prng_state *prng);
int  sprng_export(unsigned char *out, unsigned long *outlen, prng_state *prng);
int  sprng_import(const unsigned char *in, unsigned long inlen, prng_state *prng);
int  sprng_test(void);
extern const struct ltc_prng_descriptor sprng_desc;
#endif

#ifdef LTC_SOBER128
int sober128_start(prng_state *prng);
int sober128_add_entropy(const unsigned char *in, unsigned long inlen, prng_state *prng);
int sober128_ready(prng_state *prng);
unsigned long sober128_read(unsigned char *out, unsigned long outlen, prng_state *prng);
int sober128_done(prng_state *prng);
int  sober128_export(unsigned char *out, unsigned long *outlen, prng_state *prng);
int  sober128_import(const unsigned char *in, unsigned long inlen, prng_state *prng);
int  sober128_test(void);
extern const struct ltc_prng_descriptor sober128_desc;
#endif

int find_prng(const char *name);
int register_prng(const struct ltc_prng_descriptor *prng);
int unregister_prng(const struct ltc_prng_descriptor *prng);
int register_all_prngs(void);
int prng_is_valid(int idx);
LTC_MUTEX_PROTO(ltc_prng_mutex)

/* Slow RNG you **might** be able to use to seed a PRNG with.  Be careful as this
 * might not work on all platforms as planned
 */
unsigned long rng_get_bytes(unsigned char *out,
                            unsigned long outlen,
                            void (*callback)(void));

int rng_make_prng(int bits, int wprng, prng_state *prng, void (*callback)(void));

#ifdef LTC_PRNG_ENABLE_LTC_RNG
extern unsigned long (*ltc_rng)(unsigned char *out, unsigned long outlen,
      void (*callback)(void));
#endif


/* ref:         HEAD -> release/1.18.0, tag: v1.18.0-rc2 */
/* git commit:  aa0f396c0c8828ce39456129507fc72ef0208bd0 */
/* commit time: 2017-07-13 14:58:01 +0200 */
