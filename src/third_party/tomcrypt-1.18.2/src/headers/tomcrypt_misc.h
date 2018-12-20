/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

/* ---- LTC_BASE64 Routines ---- */
#ifdef LTC_BASE64
int base64_encode(const unsigned char *in,  unsigned long len,
                        unsigned char *out, unsigned long *outlen);

int base64_decode(const unsigned char *in,  unsigned long len,
                        unsigned char *out, unsigned long *outlen);
int base64_strict_decode(const unsigned char *in,  unsigned long len,
                        unsigned char *out, unsigned long *outlen);
#endif

#ifdef LTC_BASE64_URL
int base64url_encode(const unsigned char *in,  unsigned long len,
                        unsigned char *out, unsigned long *outlen);
int base64url_strict_encode(const unsigned char *in,  unsigned long inlen,
                        unsigned char *out, unsigned long *outlen);

int base64url_decode(const unsigned char *in,  unsigned long len,
                        unsigned char *out, unsigned long *outlen);
int base64url_strict_decode(const unsigned char *in,  unsigned long len,
                        unsigned char *out, unsigned long *outlen);
#endif

/* ===> LTC_HKDF -- RFC5869 HMAC-based Key Derivation Function <=== */
#ifdef LTC_HKDF

int hkdf_test(void);

int hkdf_extract(int hash_idx,
                 const unsigned char *salt, unsigned long saltlen,
                 const unsigned char *in,   unsigned long inlen,
                       unsigned char *out,  unsigned long *outlen);

int hkdf_expand(int hash_idx,
                const unsigned char *info, unsigned long infolen,
                const unsigned char *in,   unsigned long inlen,
                      unsigned char *out,  unsigned long outlen);

int hkdf(int hash_idx,
         const unsigned char *salt, unsigned long saltlen,
         const unsigned char *info, unsigned long infolen,
         const unsigned char *in,   unsigned long inlen,
               unsigned char *out,  unsigned long outlen);

#endif  /* LTC_HKDF */

/* ---- MEM routines ---- */
int mem_neq(const void *a, const void *b, size_t len);
void zeromem(volatile void *dst, size_t len);
void burn_stack(unsigned long len);

const char *error_to_string(int err);

extern const char *crypt_build_settings;

/* ---- HMM ---- */
int crypt_fsa(void *mp, ...);

/* ---- Dynamic language support ---- */
int crypt_get_constant(const char* namein, int *valueout);
int crypt_list_all_constants(char *names_list, unsigned int *names_list_size);

int crypt_get_size(const char* namein, unsigned int *sizeout);
int crypt_list_all_sizes(char *names_list, unsigned int *names_list_size);

#ifdef LTM_DESC
void init_LTM(void);
#endif
#ifdef TFM_DESC
void init_TFM(void);
#endif
#ifdef GMP_DESC
void init_GMP(void);
#endif

#ifdef LTC_ADLER32
typedef struct adler32_state_s
{
   unsigned short s[2];
} adler32_state;

void adler32_init(adler32_state *ctx);
void adler32_update(adler32_state *ctx, const unsigned char *input, unsigned long length);
void adler32_finish(adler32_state *ctx, void *hash, unsigned long size);
int adler32_test(void);
#endif

#ifdef LTC_CRC32
typedef struct crc32_state_s
{
   ulong32 crc;
} crc32_state;

void crc32_init(crc32_state *ctx);
void crc32_update(crc32_state *ctx, const unsigned char *input, unsigned long length);
void crc32_finish(crc32_state *ctx, void *hash, unsigned long size);
int crc32_test(void);
#endif

int compare_testvector(const void* is, const unsigned long is_len, const void* should, const unsigned long should_len, const char* what, int which);

/* ref:         HEAD -> master, tag: v1.18.2 */
/* git commit:  7e7eb695d581782f04b24dc444cbfde86af59853 */
/* commit time: 2018-07-01 22:49:01 +0200 */
