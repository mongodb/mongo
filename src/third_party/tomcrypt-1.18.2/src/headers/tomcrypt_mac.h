/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

#ifdef LTC_HMAC
typedef struct Hmac_state {
     hash_state     md;
     int            hash;
     hash_state     hashstate;
     unsigned char  *key;
} hmac_state;

int hmac_init(hmac_state *hmac, int hash, const unsigned char *key, unsigned long keylen);
int hmac_process(hmac_state *hmac, const unsigned char *in, unsigned long inlen);
int hmac_done(hmac_state *hmac, unsigned char *out, unsigned long *outlen);
int hmac_test(void);
int hmac_memory(int hash,
                const unsigned char *key, unsigned long keylen,
                const unsigned char *in,  unsigned long inlen,
                      unsigned char *out, unsigned long *outlen);
int hmac_memory_multi(int hash,
                const unsigned char *key,  unsigned long keylen,
                      unsigned char *out,  unsigned long *outlen,
                const unsigned char *in,   unsigned long inlen, ...);
int hmac_file(int hash, const char *fname, const unsigned char *key,
              unsigned long keylen,
              unsigned char *dst, unsigned long *dstlen);
#endif

#ifdef LTC_OMAC

typedef struct {
   int             cipher_idx,
                   buflen,
                   blklen;
   unsigned char   block[MAXBLOCKSIZE],
                   prev[MAXBLOCKSIZE],
                   Lu[2][MAXBLOCKSIZE];
   symmetric_key   key;
} omac_state;

int omac_init(omac_state *omac, int cipher, const unsigned char *key, unsigned long keylen);
int omac_process(omac_state *omac, const unsigned char *in, unsigned long inlen);
int omac_done(omac_state *omac, unsigned char *out, unsigned long *outlen);
int omac_memory(int cipher,
               const unsigned char *key, unsigned long keylen,
               const unsigned char *in,  unsigned long inlen,
                     unsigned char *out, unsigned long *outlen);
int omac_memory_multi(int cipher,
                const unsigned char *key, unsigned long keylen,
                      unsigned char *out, unsigned long *outlen,
                const unsigned char *in,  unsigned long inlen, ...);
int omac_file(int cipher,
              const unsigned char *key, unsigned long keylen,
              const          char *filename,
                    unsigned char *out, unsigned long *outlen);
int omac_test(void);
#endif /* LTC_OMAC */

#ifdef LTC_PMAC

typedef struct {
   unsigned char     Ls[32][MAXBLOCKSIZE],    /* L shifted by i bits to the left */
                     Li[MAXBLOCKSIZE],        /* value of Li [current value, we calc from previous recall] */
                     Lr[MAXBLOCKSIZE],        /* L * x^-1 */
                     block[MAXBLOCKSIZE],     /* currently accumulated block */
                     checksum[MAXBLOCKSIZE];  /* current checksum */

   symmetric_key     key;                     /* scheduled key for cipher */
   unsigned long     block_index;             /* index # for current block */
   int               cipher_idx,              /* cipher idx */
                     block_len,               /* length of block */
                     buflen;                  /* number of bytes in the buffer */
} pmac_state;

int pmac_init(pmac_state *pmac, int cipher, const unsigned char *key, unsigned long keylen);
int pmac_process(pmac_state *pmac, const unsigned char *in, unsigned long inlen);
int pmac_done(pmac_state *pmac, unsigned char *out, unsigned long *outlen);

int pmac_memory(int cipher,
               const unsigned char *key, unsigned long keylen,
               const unsigned char *msg, unsigned long msglen,
                     unsigned char *out, unsigned long *outlen);

int pmac_memory_multi(int cipher,
                const unsigned char *key, unsigned long keylen,
                      unsigned char *out, unsigned long *outlen,
                const unsigned char *in, unsigned long inlen, ...);

int pmac_file(int cipher,
             const unsigned char *key, unsigned long keylen,
             const          char *filename,
                   unsigned char *out, unsigned long *outlen);

int pmac_test(void);

/* internal functions */
int pmac_ntz(unsigned long x);
void pmac_shift_xor(pmac_state *pmac);

#endif /* PMAC */

#ifdef LTC_POLY1305
typedef struct {
   ulong32 r[5];
   ulong32 h[5];
   ulong32 pad[4];
   unsigned long leftover;
   unsigned char buffer[16];
   int final;
} poly1305_state;

int poly1305_init(poly1305_state *st, const unsigned char *key, unsigned long keylen);
int poly1305_process(poly1305_state *st, const unsigned char *in, unsigned long inlen);
int poly1305_done(poly1305_state *st, unsigned char *mac, unsigned long *maclen);
int poly1305_memory(const unsigned char *key, unsigned long keylen, const unsigned char *in, unsigned long inlen, unsigned char *mac, unsigned long *maclen);
int poly1305_memory_multi(const unsigned char *key, unsigned long keylen, unsigned char *mac, unsigned long *maclen, const unsigned char *in,  unsigned long inlen, ...);
int poly1305_file(const char *fname, const unsigned char *key, unsigned long keylen, unsigned char *mac, unsigned long *maclen);
int poly1305_test(void);
#endif /* LTC_POLY1305 */

#ifdef LTC_BLAKE2SMAC
typedef hash_state blake2smac_state;
int blake2smac_init(blake2smac_state *st, unsigned long outlen, const unsigned char *key, unsigned long keylen);
int blake2smac_process(blake2smac_state *st, const unsigned char *in, unsigned long inlen);
int blake2smac_done(blake2smac_state *st, unsigned char *mac, unsigned long *maclen);
int blake2smac_memory(const unsigned char *key, unsigned long keylen, const unsigned char *in, unsigned long inlen, unsigned char *mac, unsigned long *maclen);
int blake2smac_memory_multi(const unsigned char *key, unsigned long keylen, unsigned char *mac, unsigned long *maclen, const unsigned char *in,  unsigned long inlen, ...);
int blake2smac_file(const char *fname, const unsigned char *key, unsigned long keylen, unsigned char *mac, unsigned long *maclen);
int blake2smac_test(void);
#endif /* LTC_BLAKE2SMAC */

#ifdef LTC_BLAKE2BMAC
typedef hash_state blake2bmac_state;
int blake2bmac_init(blake2bmac_state *st, unsigned long outlen, const unsigned char *key, unsigned long keylen);
int blake2bmac_process(blake2bmac_state *st, const unsigned char *in, unsigned long inlen);
int blake2bmac_done(blake2bmac_state *st, unsigned char *mac, unsigned long *maclen);
int blake2bmac_memory(const unsigned char *key, unsigned long keylen, const unsigned char *in, unsigned long inlen, unsigned char *mac, unsigned long *maclen);
int blake2bmac_memory_multi(const unsigned char *key, unsigned long keylen, unsigned char *mac, unsigned long *maclen, const unsigned char *in,  unsigned long inlen, ...);
int blake2bmac_file(const char *fname, const unsigned char *key, unsigned long keylen, unsigned char *mac, unsigned long *maclen);
int blake2bmac_test(void);
#endif /* LTC_BLAKE2BMAC */

#ifdef LTC_EAX_MODE

#if !(defined(LTC_OMAC) && defined(LTC_CTR_MODE))
   #error LTC_EAX_MODE requires LTC_OMAC and CTR
#endif

typedef struct {
   unsigned char N[MAXBLOCKSIZE];
   symmetric_CTR ctr;
   omac_state    headeromac, ctomac;
} eax_state;

int eax_init(eax_state *eax, int cipher, const unsigned char *key, unsigned long keylen,
             const unsigned char *nonce, unsigned long noncelen,
             const unsigned char *header, unsigned long headerlen);

int eax_encrypt(eax_state *eax, const unsigned char *pt, unsigned char *ct, unsigned long length);
int eax_decrypt(eax_state *eax, const unsigned char *ct, unsigned char *pt, unsigned long length);
int eax_addheader(eax_state *eax, const unsigned char *header, unsigned long length);
int eax_done(eax_state *eax, unsigned char *tag, unsigned long *taglen);

int eax_encrypt_authenticate_memory(int cipher,
    const unsigned char *key,    unsigned long keylen,
    const unsigned char *nonce,  unsigned long noncelen,
    const unsigned char *header, unsigned long headerlen,
    const unsigned char *pt,     unsigned long ptlen,
          unsigned char *ct,
          unsigned char *tag,    unsigned long *taglen);

int eax_decrypt_verify_memory(int cipher,
    const unsigned char *key,    unsigned long keylen,
    const unsigned char *nonce,  unsigned long noncelen,
    const unsigned char *header, unsigned long headerlen,
    const unsigned char *ct,     unsigned long ctlen,
          unsigned char *pt,
          unsigned char *tag,    unsigned long taglen,
          int           *stat);

 int eax_test(void);
#endif /* EAX MODE */

#ifdef LTC_OCB_MODE
typedef struct {
   unsigned char     L[MAXBLOCKSIZE],         /* L value */
                     Ls[32][MAXBLOCKSIZE],    /* L shifted by i bits to the left */
                     Li[MAXBLOCKSIZE],        /* value of Li [current value, we calc from previous recall] */
                     Lr[MAXBLOCKSIZE],        /* L * x^-1 */
                     R[MAXBLOCKSIZE],         /* R value */
                     checksum[MAXBLOCKSIZE];  /* current checksum */

   symmetric_key     key;                     /* scheduled key for cipher */
   unsigned long     block_index;             /* index # for current block */
   int               cipher,                  /* cipher idx */
                     block_len;               /* length of block */
} ocb_state;

int ocb_init(ocb_state *ocb, int cipher,
             const unsigned char *key, unsigned long keylen, const unsigned char *nonce);

int ocb_encrypt(ocb_state *ocb, const unsigned char *pt, unsigned char *ct);
int ocb_decrypt(ocb_state *ocb, const unsigned char *ct, unsigned char *pt);

int ocb_done_encrypt(ocb_state *ocb,
                     const unsigned char *pt,  unsigned long ptlen,
                           unsigned char *ct,
                           unsigned char *tag, unsigned long *taglen);

int ocb_done_decrypt(ocb_state *ocb,
                     const unsigned char *ct,  unsigned long ctlen,
                           unsigned char *pt,
                     const unsigned char *tag, unsigned long taglen, int *stat);

int ocb_encrypt_authenticate_memory(int cipher,
    const unsigned char *key,    unsigned long keylen,
    const unsigned char *nonce,
    const unsigned char *pt,     unsigned long ptlen,
          unsigned char *ct,
          unsigned char *tag,    unsigned long *taglen);

int ocb_decrypt_verify_memory(int cipher,
    const unsigned char *key,    unsigned long keylen,
    const unsigned char *nonce,
    const unsigned char *ct,     unsigned long ctlen,
          unsigned char *pt,
    const unsigned char *tag,    unsigned long taglen,
          int           *stat);

int ocb_test(void);

/* internal functions */
void ocb_shift_xor(ocb_state *ocb, unsigned char *Z);
int ocb_ntz(unsigned long x);
int s_ocb_done(ocb_state *ocb, const unsigned char *pt, unsigned long ptlen,
               unsigned char *ct, unsigned char *tag, unsigned long *taglen, int mode);

#endif /* LTC_OCB_MODE */

#ifdef LTC_OCB3_MODE
typedef struct {
   unsigned char     Offset_0[MAXBLOCKSIZE],       /* Offset_0 value */
                     Offset_current[MAXBLOCKSIZE], /* Offset_{current_block_index} value */
                     L_dollar[MAXBLOCKSIZE],       /* L_$ value */
                     L_star[MAXBLOCKSIZE],         /* L_* value */
                     L_[32][MAXBLOCKSIZE],         /* L_{i} values */
                     tag_part[MAXBLOCKSIZE],       /* intermediate result of tag calculation */
                     checksum[MAXBLOCKSIZE];       /* current checksum */

   /* AAD related members */
   unsigned char     aSum_current[MAXBLOCKSIZE],    /* AAD related helper variable */
                     aOffset_current[MAXBLOCKSIZE], /* AAD related helper variable */
                     adata_buffer[MAXBLOCKSIZE];    /* AAD buffer */
   int               adata_buffer_bytes;            /* bytes in AAD buffer */
   unsigned long     ablock_index;                  /* index # for current adata (AAD) block */

   symmetric_key     key;                     /* scheduled key for cipher */
   unsigned long     block_index;             /* index # for current data block */
   int               cipher,                  /* cipher idx */
                     tag_len,                 /* length of tag */
                     block_len;               /* length of block */
} ocb3_state;

int ocb3_init(ocb3_state *ocb, int cipher,
             const unsigned char *key, unsigned long keylen,
             const unsigned char *nonce, unsigned long noncelen,
             unsigned long taglen);

int ocb3_encrypt(ocb3_state *ocb, const unsigned char *pt, unsigned long ptlen, unsigned char *ct);
int ocb3_decrypt(ocb3_state *ocb, const unsigned char *ct, unsigned long ctlen, unsigned char *pt);
int ocb3_encrypt_last(ocb3_state *ocb, const unsigned char *pt, unsigned long ptlen, unsigned char *ct);
int ocb3_decrypt_last(ocb3_state *ocb, const unsigned char *ct, unsigned long ctlen, unsigned char *pt);
int ocb3_add_aad(ocb3_state *ocb, const unsigned char *aad, unsigned long aadlen);
int ocb3_done(ocb3_state *ocb, unsigned char *tag, unsigned long *taglen);

int ocb3_encrypt_authenticate_memory(int cipher,
    const unsigned char *key,    unsigned long keylen,
    const unsigned char *nonce,  unsigned long noncelen,
    const unsigned char *adata,  unsigned long adatalen,
    const unsigned char *pt,     unsigned long ptlen,
          unsigned char *ct,
          unsigned char *tag,    unsigned long *taglen);

int ocb3_decrypt_verify_memory(int cipher,
    const unsigned char *key,    unsigned long keylen,
    const unsigned char *nonce,  unsigned long noncelen,
    const unsigned char *adata,  unsigned long adatalen,
    const unsigned char *ct,     unsigned long ctlen,
          unsigned char *pt,
    const unsigned char *tag,    unsigned long taglen,
          int           *stat);

int ocb3_test(void);

#ifdef LTC_SOURCE
/* internal helper functions */
int ocb3_int_ntz(unsigned long x);
void ocb3_int_xor_blocks(unsigned char *out, const unsigned char *block_a, const unsigned char *block_b, unsigned long block_len);
#endif /* LTC_SOURCE */

#endif /* LTC_OCB3_MODE */

#ifdef LTC_CCM_MODE

#define CCM_ENCRYPT LTC_ENCRYPT
#define CCM_DECRYPT LTC_DECRYPT

typedef struct {
   symmetric_key       K;
   int                 cipher,               /* which cipher */
                       taglen,               /* length of the tag */
                       x;                    /* index in PAD */

   unsigned long       L,                    /* L value */
                       ptlen,                /* length that will be enc / dec */
                       current_ptlen,        /* current processed length */
                       aadlen,               /* length of the aad */
                       current_aadlen,       /* length of the currently provided add */
                       noncelen;             /* length of the nonce */

   unsigned char       PAD[16],
                       ctr[16],
                       CTRPAD[16],
                       CTRlen;
} ccm_state;

int ccm_init(ccm_state *ccm, int cipher,
             const unsigned char *key, int keylen, int ptlen, int taglen, int aad_len);

int ccm_reset(ccm_state *ccm);

int ccm_add_nonce(ccm_state *ccm,
                  const unsigned char *nonce,     unsigned long noncelen);

int ccm_add_aad(ccm_state *ccm,
                const unsigned char *adata,  unsigned long adatalen);

int ccm_process(ccm_state *ccm,
                unsigned char *pt,     unsigned long ptlen,
                unsigned char *ct,
                int direction);

int ccm_done(ccm_state *ccm,
             unsigned char *tag,    unsigned long *taglen);

int ccm_memory(int cipher,
    const unsigned char *key,    unsigned long keylen,
    symmetric_key       *uskey,
    const unsigned char *nonce,  unsigned long noncelen,
    const unsigned char *header, unsigned long headerlen,
          unsigned char *pt,     unsigned long ptlen,
          unsigned char *ct,
          unsigned char *tag,    unsigned long *taglen,
                    int  direction);

int ccm_test(void);

#endif /* LTC_CCM_MODE */

#if defined(LRW_MODE) || defined(LTC_GCM_MODE)
void gcm_gf_mult(const unsigned char *a, const unsigned char *b, unsigned char *c);
#endif


/* table shared between GCM and LRW */
#if defined(LTC_GCM_TABLES) || defined(LTC_LRW_TABLES) || ((defined(LTC_GCM_MODE) || defined(LTC_GCM_MODE)) && defined(LTC_FAST))
extern const unsigned char gcm_shift_table[];
#endif

#ifdef LTC_GCM_MODE

#define GCM_ENCRYPT LTC_ENCRYPT
#define GCM_DECRYPT LTC_DECRYPT

#define LTC_GCM_MODE_IV    0
#define LTC_GCM_MODE_AAD   1
#define LTC_GCM_MODE_TEXT  2

typedef struct {
   symmetric_key       K;
   unsigned char       H[16],        /* multiplier */
                       X[16],        /* accumulator */
                       Y[16],        /* counter */
                       Y_0[16],      /* initial counter */
                       buf[16];      /* buffer for stuff */

   int                 cipher,       /* which cipher */
                       ivmode,       /* Which mode is the IV in? */
                       mode,         /* mode the GCM code is in */
                       buflen;       /* length of data in buf */

   ulong64             totlen,       /* 64-bit counter used for IV and AAD */
                       pttotlen;     /* 64-bit counter for the PT */

#ifdef LTC_GCM_TABLES
   unsigned char       PC[16][256][16]  /* 16 tables of 8x128 */
#ifdef LTC_GCM_TABLES_SSE2
__attribute__ ((aligned (16)))
#endif
;
#endif
} gcm_state;

void gcm_mult_h(gcm_state *gcm, unsigned char *I);

int gcm_init(gcm_state *gcm, int cipher,
             const unsigned char *key, int keylen);

int gcm_reset(gcm_state *gcm);

int gcm_add_iv(gcm_state *gcm,
               const unsigned char *IV,     unsigned long IVlen);

int gcm_add_aad(gcm_state *gcm,
               const unsigned char *adata,  unsigned long adatalen);

int gcm_process(gcm_state *gcm,
                     unsigned char *pt,     unsigned long ptlen,
                     unsigned char *ct,
                     int direction);

int gcm_done(gcm_state *gcm,
                     unsigned char *tag,    unsigned long *taglen);

int gcm_memory(      int           cipher,
               const unsigned char *key,    unsigned long keylen,
               const unsigned char *IV,     unsigned long IVlen,
               const unsigned char *adata,  unsigned long adatalen,
                     unsigned char *pt,     unsigned long ptlen,
                     unsigned char *ct,
                     unsigned char *tag,    unsigned long *taglen,
                               int direction);
int gcm_test(void);

#endif /* LTC_GCM_MODE */

#ifdef LTC_PELICAN

typedef struct pelican_state
{
    symmetric_key K;
    unsigned char state[16];
    int           buflen;
} pelican_state;

int pelican_init(pelican_state *pelmac, const unsigned char *key, unsigned long keylen);
int pelican_process(pelican_state *pelmac, const unsigned char *in, unsigned long inlen);
int pelican_done(pelican_state *pelmac, unsigned char *out);
int pelican_test(void);

int pelican_memory(const unsigned char *key, unsigned long keylen,
                   const unsigned char *in, unsigned long inlen,
                         unsigned char *out);

#endif

#ifdef LTC_XCBC

/* add this to "keylen" to xcbc_init to use a pure three-key XCBC MAC */
#define LTC_XCBC_PURE  0x8000UL

typedef struct {
   unsigned char K[3][MAXBLOCKSIZE],
                 IV[MAXBLOCKSIZE];

   symmetric_key key;

             int cipher,
                 buflen,
                 blocksize;
} xcbc_state;

int xcbc_init(xcbc_state *xcbc, int cipher, const unsigned char *key, unsigned long keylen);
int xcbc_process(xcbc_state *xcbc, const unsigned char *in, unsigned long inlen);
int xcbc_done(xcbc_state *xcbc, unsigned char *out, unsigned long *outlen);
int xcbc_memory(int cipher,
               const unsigned char *key, unsigned long keylen,
               const unsigned char *in,  unsigned long inlen,
                     unsigned char *out, unsigned long *outlen);
int xcbc_memory_multi(int cipher,
                const unsigned char *key, unsigned long keylen,
                      unsigned char *out, unsigned long *outlen,
                const unsigned char *in,  unsigned long inlen, ...);
int xcbc_file(int cipher,
              const unsigned char *key, unsigned long keylen,
              const          char *filename,
                    unsigned char *out, unsigned long *outlen);
int xcbc_test(void);

#endif

#ifdef LTC_F9_MODE

typedef struct {
   unsigned char akey[MAXBLOCKSIZE],
                 ACC[MAXBLOCKSIZE],
                 IV[MAXBLOCKSIZE];

   symmetric_key key;

             int cipher,
                 buflen,
                 keylen,
                 blocksize;
} f9_state;

int f9_init(f9_state *f9, int cipher, const unsigned char *key, unsigned long keylen);
int f9_process(f9_state *f9, const unsigned char *in, unsigned long inlen);
int f9_done(f9_state *f9, unsigned char *out, unsigned long *outlen);
int f9_memory(int cipher,
               const unsigned char *key, unsigned long keylen,
               const unsigned char *in,  unsigned long inlen,
                     unsigned char *out, unsigned long *outlen);
int f9_memory_multi(int cipher,
                const unsigned char *key, unsigned long keylen,
                      unsigned char *out, unsigned long *outlen,
                const unsigned char *in,  unsigned long inlen, ...);
int f9_file(int cipher,
              const unsigned char *key, unsigned long keylen,
              const          char *filename,
                    unsigned char *out, unsigned long *outlen);
int f9_test(void);

#endif

#ifdef LTC_CHACHA20POLY1305_MODE

typedef struct {
   poly1305_state poly;
   chacha_state chacha;
   ulong64 aadlen;
   ulong64 ctlen;
   int aadflg;
} chacha20poly1305_state;

#define CHACHA20POLY1305_ENCRYPT LTC_ENCRYPT
#define CHACHA20POLY1305_DECRYPT LTC_DECRYPT

int chacha20poly1305_init(chacha20poly1305_state *st, const unsigned char *key, unsigned long keylen);
int chacha20poly1305_setiv(chacha20poly1305_state *st, const unsigned char *iv, unsigned long ivlen);
int chacha20poly1305_setiv_rfc7905(chacha20poly1305_state *st, const unsigned char *iv, unsigned long ivlen, ulong64 sequence_number);
int chacha20poly1305_add_aad(chacha20poly1305_state *st, const unsigned char *in, unsigned long inlen);
int chacha20poly1305_encrypt(chacha20poly1305_state *st, const unsigned char *in, unsigned long inlen, unsigned char *out);
int chacha20poly1305_decrypt(chacha20poly1305_state *st, const unsigned char *in, unsigned long inlen, unsigned char *out);
int chacha20poly1305_done(chacha20poly1305_state *st, unsigned char *tag, unsigned long *taglen);
int chacha20poly1305_memory(const unsigned char *key, unsigned long keylen,
                            const unsigned char *iv,  unsigned long ivlen,
                            const unsigned char *aad, unsigned long aadlen,
                            const unsigned char *in,  unsigned long inlen,
                                  unsigned char *out,
                                  unsigned char *tag, unsigned long *taglen,
                            int direction);
int chacha20poly1305_test(void);

#endif /* LTC_CHACHA20POLY1305_MODE */

/* ref:         HEAD -> master, tag: v1.18.2 */
/* git commit:  7e7eb695d581782f04b24dc444cbfde86af59853 */
/* commit time: 2018-07-01 22:49:01 +0200 */
