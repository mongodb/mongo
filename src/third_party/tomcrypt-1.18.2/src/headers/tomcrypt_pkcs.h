/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

/* PKCS Header Info */

/* ===> PKCS #1 -- RSA Cryptography <=== */
#ifdef LTC_PKCS_1

enum ltc_pkcs_1_v1_5_blocks
{
  LTC_PKCS_1_EMSA   = 1,        /* Block type 1 (PKCS #1 v1.5 signature padding) */
  LTC_PKCS_1_EME    = 2         /* Block type 2 (PKCS #1 v1.5 encryption padding) */
};

enum ltc_pkcs_1_paddings
{
  LTC_PKCS_1_V1_5     = 1,        /* PKCS #1 v1.5 padding (\sa ltc_pkcs_1_v1_5_blocks) */
  LTC_PKCS_1_OAEP     = 2,        /* PKCS #1 v2.0 encryption padding */
  LTC_PKCS_1_PSS      = 3,        /* PKCS #1 v2.1 signature padding */
  LTC_PKCS_1_V1_5_NA1 = 4         /* PKCS #1 v1.5 padding - No ASN.1 (\sa ltc_pkcs_1_v1_5_blocks) */
};

int pkcs_1_mgf1(      int            hash_idx,
                const unsigned char *seed, unsigned long seedlen,
                      unsigned char *mask, unsigned long masklen);

int pkcs_1_i2osp(void *n, unsigned long modulus_len, unsigned char *out);
int pkcs_1_os2ip(void *n, unsigned char *in, unsigned long inlen);

/* *** v1.5 padding */
int pkcs_1_v1_5_encode(const unsigned char *msg,
                             unsigned long  msglen,
                             int            block_type,
                             unsigned long  modulus_bitlen,
                                prng_state *prng,
                                       int  prng_idx,
                             unsigned char *out,
                             unsigned long *outlen);

int pkcs_1_v1_5_decode(const unsigned char *msg,
                             unsigned long  msglen,
                                       int  block_type,
                             unsigned long  modulus_bitlen,
                             unsigned char *out,
                             unsigned long *outlen,
                                       int *is_valid);

/* *** v2.1 padding */
int pkcs_1_oaep_encode(const unsigned char *msg,    unsigned long msglen,
                       const unsigned char *lparam, unsigned long lparamlen,
                             unsigned long modulus_bitlen, prng_state *prng,
                             int           prng_idx,         int  hash_idx,
                             unsigned char *out,    unsigned long *outlen);

int pkcs_1_oaep_decode(const unsigned char *msg,    unsigned long msglen,
                       const unsigned char *lparam, unsigned long lparamlen,
                             unsigned long modulus_bitlen, int hash_idx,
                             unsigned char *out,    unsigned long *outlen,
                             int           *res);

int pkcs_1_pss_encode(const unsigned char *msghash, unsigned long msghashlen,
                            unsigned long saltlen,  prng_state   *prng,
                            int           prng_idx, int           hash_idx,
                            unsigned long modulus_bitlen,
                            unsigned char *out,     unsigned long *outlen);

int pkcs_1_pss_decode(const unsigned char *msghash, unsigned long msghashlen,
                      const unsigned char *sig,     unsigned long siglen,
                            unsigned long saltlen,  int           hash_idx,
                            unsigned long modulus_bitlen, int    *res);

#endif /* LTC_PKCS_1 */

/* ===> PKCS #5 -- Password Based Cryptography <=== */
#ifdef LTC_PKCS_5

/* Algorithm #1 (PBKDF1) */
int pkcs_5_alg1(const unsigned char *password, unsigned long password_len,
                const unsigned char *salt,
                int iteration_count,  int hash_idx,
                unsigned char *out,   unsigned long *outlen);

/* Algorithm #1 (PBKDF1) - OpenSSL-compatible variant for arbitrarily-long keys.
   Compatible with EVP_BytesToKey() */
int pkcs_5_alg1_openssl(const unsigned char *password,
                        unsigned long password_len,
                        const unsigned char *salt,
                        int iteration_count,  int hash_idx,
                        unsigned char *out,   unsigned long *outlen);

/* Algorithm #2 (PBKDF2) */
int pkcs_5_alg2(const unsigned char *password, unsigned long password_len,
                const unsigned char *salt,     unsigned long salt_len,
                int iteration_count,           int hash_idx,
                unsigned char *out,            unsigned long *outlen);

int pkcs_5_test (void);
#endif  /* LTC_PKCS_5 */

/* ref:         HEAD -> master, tag: v1.18.2 */
/* git commit:  7e7eb695d581782f04b24dc444cbfde86af59853 */
/* commit time: 2018-07-01 22:49:01 +0200 */
