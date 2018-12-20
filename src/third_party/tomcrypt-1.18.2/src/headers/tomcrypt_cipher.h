/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

/* ---- SYMMETRIC KEY STUFF -----
 *
 * We put each of the ciphers scheduled keys in their own structs then we put all of
 * the key formats in one union.  This makes the function prototypes easier to use.
 */
#ifdef LTC_BLOWFISH
struct blowfish_key {
   ulong32 S[4][256];
   ulong32 K[18];
};
#endif

#ifdef LTC_RC5
struct rc5_key {
   int rounds;
   ulong32 K[50];
};
#endif

#ifdef LTC_RC6
struct rc6_key {
   ulong32 K[44];
};
#endif

#ifdef LTC_SAFERP
struct saferp_key {
   unsigned char K[33][16];
   long rounds;
};
#endif

#ifdef LTC_RIJNDAEL
struct rijndael_key {
   ulong32 eK[60], dK[60];
   int Nr;
};
#endif

#ifdef LTC_KSEED
struct kseed_key {
    ulong32 K[32], dK[32];
};
#endif

#ifdef LTC_KASUMI
struct kasumi_key {
    ulong32 KLi1[8], KLi2[8],
            KOi1[8], KOi2[8], KOi3[8],
            KIi1[8], KIi2[8], KIi3[8];
};
#endif

#ifdef LTC_XTEA
struct xtea_key {
   unsigned long A[32], B[32];
};
#endif

#ifdef LTC_TWOFISH
#ifndef LTC_TWOFISH_SMALL
   struct twofish_key {
      ulong32 S[4][256], K[40];
   };
#else
   struct twofish_key {
      ulong32 K[40];
      unsigned char S[32], start;
   };
#endif
#endif

#ifdef LTC_SAFER
#define LTC_SAFER_K64_DEFAULT_NOF_ROUNDS     6
#define LTC_SAFER_K128_DEFAULT_NOF_ROUNDS   10
#define LTC_SAFER_SK64_DEFAULT_NOF_ROUNDS    8
#define LTC_SAFER_SK128_DEFAULT_NOF_ROUNDS  10
#define LTC_SAFER_MAX_NOF_ROUNDS            13
#define LTC_SAFER_BLOCK_LEN                  8
#define LTC_SAFER_KEY_LEN     (1 + LTC_SAFER_BLOCK_LEN * (1 + 2 * LTC_SAFER_MAX_NOF_ROUNDS))
typedef unsigned char safer_block_t[LTC_SAFER_BLOCK_LEN];
typedef unsigned char safer_key_t[LTC_SAFER_KEY_LEN];
struct safer_key { safer_key_t key; };
#endif

#ifdef LTC_RC2
struct rc2_key { unsigned xkey[64]; };
#endif

#ifdef LTC_DES
struct des_key {
    ulong32 ek[32], dk[32];
};

struct des3_key {
    ulong32 ek[3][32], dk[3][32];
};
#endif

#ifdef LTC_CAST5
struct cast5_key {
    ulong32 K[32], keylen;
};
#endif

#ifdef LTC_NOEKEON
struct noekeon_key {
    ulong32 K[4], dK[4];
};
#endif

#ifdef LTC_SKIPJACK
struct skipjack_key {
    unsigned char key[10];
};
#endif

#ifdef LTC_KHAZAD
struct khazad_key {
   ulong64 roundKeyEnc[8 + 1];
   ulong64 roundKeyDec[8 + 1];
};
#endif

#ifdef LTC_ANUBIS
struct anubis_key {
   int keyBits;
   int R;
   ulong32 roundKeyEnc[18 + 1][4];
   ulong32 roundKeyDec[18 + 1][4];
};
#endif

#ifdef LTC_MULTI2
struct multi2_key {
    int N;
    ulong32 uk[8];
};
#endif

#ifdef LTC_CAMELLIA
struct camellia_key {
    int R;
    ulong64 kw[4], k[24], kl[6];
};
#endif

typedef union Symmetric_key {
#ifdef LTC_DES
   struct des_key des;
   struct des3_key des3;
#endif
#ifdef LTC_RC2
   struct rc2_key rc2;
#endif
#ifdef LTC_SAFER
   struct safer_key safer;
#endif
#ifdef LTC_TWOFISH
   struct twofish_key  twofish;
#endif
#ifdef LTC_BLOWFISH
   struct blowfish_key blowfish;
#endif
#ifdef LTC_RC5
   struct rc5_key      rc5;
#endif
#ifdef LTC_RC6
   struct rc6_key      rc6;
#endif
#ifdef LTC_SAFERP
   struct saferp_key   saferp;
#endif
#ifdef LTC_RIJNDAEL
   struct rijndael_key rijndael;
#endif
#ifdef LTC_XTEA
   struct xtea_key     xtea;
#endif
#ifdef LTC_CAST5
   struct cast5_key    cast5;
#endif
#ifdef LTC_NOEKEON
   struct noekeon_key  noekeon;
#endif
#ifdef LTC_SKIPJACK
   struct skipjack_key skipjack;
#endif
#ifdef LTC_KHAZAD
   struct khazad_key   khazad;
#endif
#ifdef LTC_ANUBIS
   struct anubis_key   anubis;
#endif
#ifdef LTC_KSEED
   struct kseed_key    kseed;
#endif
#ifdef LTC_KASUMI
   struct kasumi_key   kasumi;
#endif
#ifdef LTC_MULTI2
   struct multi2_key   multi2;
#endif
#ifdef LTC_CAMELLIA
   struct camellia_key camellia;
#endif
   void   *data;
} symmetric_key;

#ifdef LTC_ECB_MODE
/** A block cipher ECB structure */
typedef struct {
   /** The index of the cipher chosen */
   int                 cipher,
   /** The block size of the given cipher */
                       blocklen;
   /** The scheduled key */
   symmetric_key       key;
} symmetric_ECB;
#endif

#ifdef LTC_CFB_MODE
/** A block cipher CFB structure */
typedef struct {
   /** The index of the cipher chosen */
   int                 cipher,
   /** The block size of the given cipher */
                       blocklen,
   /** The padding offset */
                       padlen;
   /** The current IV */
   unsigned char       IV[MAXBLOCKSIZE],
   /** The pad used to encrypt/decrypt */
                       pad[MAXBLOCKSIZE];
   /** The scheduled key */
   symmetric_key       key;
} symmetric_CFB;
#endif

#ifdef LTC_OFB_MODE
/** A block cipher OFB structure */
typedef struct {
   /** The index of the cipher chosen */
   int                 cipher,
   /** The block size of the given cipher */
                       blocklen,
   /** The padding offset */
                       padlen;
   /** The current IV */
   unsigned char       IV[MAXBLOCKSIZE];
   /** The scheduled key */
   symmetric_key       key;
} symmetric_OFB;
#endif

#ifdef LTC_CBC_MODE
/** A block cipher CBC structure */
typedef struct {
   /** The index of the cipher chosen */
   int                 cipher,
   /** The block size of the given cipher */
                       blocklen;
   /** The current IV */
   unsigned char       IV[MAXBLOCKSIZE];
   /** The scheduled key */
   symmetric_key       key;
} symmetric_CBC;
#endif


#ifdef LTC_CTR_MODE
/** A block cipher CTR structure */
typedef struct {
   /** The index of the cipher chosen */
   int                 cipher,
   /** The block size of the given cipher */
                       blocklen,
   /** The padding offset */
                       padlen,
   /** The mode (endianess) of the CTR, 0==little, 1==big */
                       mode,
   /** counter width */
                       ctrlen;

   /** The counter */
   unsigned char       ctr[MAXBLOCKSIZE],
   /** The pad used to encrypt/decrypt */
                       pad[MAXBLOCKSIZE];
   /** The scheduled key */
   symmetric_key       key;
} symmetric_CTR;
#endif


#ifdef LTC_LRW_MODE
/** A LRW structure */
typedef struct {
    /** The index of the cipher chosen (must be a 128-bit block cipher) */
    int               cipher;

    /** The current IV */
    unsigned char     IV[16],

    /** the tweak key */
                      tweak[16],

    /** The current pad, it's the product of the first 15 bytes against the tweak key */
                      pad[16];

    /** The scheduled symmetric key */
    symmetric_key     key;

#ifdef LTC_LRW_TABLES
    /** The pre-computed multiplication table */
    unsigned char     PC[16][256][16];
#endif
} symmetric_LRW;
#endif

#ifdef LTC_F8_MODE
/** A block cipher F8 structure */
typedef struct {
   /** The index of the cipher chosen */
   int                 cipher,
   /** The block size of the given cipher */
                       blocklen,
   /** The padding offset */
                       padlen;
   /** The current IV */
   unsigned char       IV[MAXBLOCKSIZE],
                       MIV[MAXBLOCKSIZE];
   /** Current block count */
   ulong32             blockcnt;
   /** The scheduled key */
   symmetric_key       key;
} symmetric_F8;
#endif


/** cipher descriptor table, last entry has "name == NULL" to mark the end of table */
extern struct ltc_cipher_descriptor {
   /** name of cipher */
   const char *name;
   /** internal ID */
   unsigned char ID;
   /** min keysize (octets) */
   int  min_key_length,
   /** max keysize (octets) */
        max_key_length,
   /** block size (octets) */
        block_length,
   /** default number of rounds */
        default_rounds;
   /** Setup the cipher
      @param key         The input symmetric key
      @param keylen      The length of the input key (octets)
      @param num_rounds  The requested number of rounds (0==default)
      @param skey        [out] The destination of the scheduled key
      @return CRYPT_OK if successful
   */
   int  (*setup)(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
   /** Encrypt a block
      @param pt      The plaintext
      @param ct      [out] The ciphertext
      @param skey    The scheduled key
      @return CRYPT_OK if successful
   */
   int (*ecb_encrypt)(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
   /** Decrypt a block
      @param ct      The ciphertext
      @param pt      [out] The plaintext
      @param skey    The scheduled key
      @return CRYPT_OK if successful
   */
   int (*ecb_decrypt)(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
   /** Test the block cipher
       @return CRYPT_OK if successful, CRYPT_NOP if self-testing has been disabled
   */
   int (*test)(void);

   /** Terminate the context
      @param skey    The scheduled key
   */
   void (*done)(symmetric_key *skey);

   /** Determine a key size
       @param keysize    [in/out] The size of the key desired and the suggested size
       @return CRYPT_OK if successful
   */
   int  (*keysize)(int *keysize);

/** Accelerators **/
   /** Accelerated ECB encryption
       @param pt      Plaintext
       @param ct      Ciphertext
       @param blocks  The number of complete blocks to process
       @param skey    The scheduled key context
       @return CRYPT_OK if successful
   */
   int (*accel_ecb_encrypt)(const unsigned char *pt, unsigned char *ct, unsigned long blocks, symmetric_key *skey);

   /** Accelerated ECB decryption
       @param pt      Plaintext
       @param ct      Ciphertext
       @param blocks  The number of complete blocks to process
       @param skey    The scheduled key context
       @return CRYPT_OK if successful
   */
   int (*accel_ecb_decrypt)(const unsigned char *ct, unsigned char *pt, unsigned long blocks, symmetric_key *skey);

   /** Accelerated CBC encryption
       @param pt      Plaintext
       @param ct      Ciphertext
       @param blocks  The number of complete blocks to process
       @param IV      The initial value (input/output)
       @param skey    The scheduled key context
       @return CRYPT_OK if successful
   */
   int (*accel_cbc_encrypt)(const unsigned char *pt, unsigned char *ct, unsigned long blocks, unsigned char *IV, symmetric_key *skey);

   /** Accelerated CBC decryption
       @param pt      Plaintext
       @param ct      Ciphertext
       @param blocks  The number of complete blocks to process
       @param IV      The initial value (input/output)
       @param skey    The scheduled key context
       @return CRYPT_OK if successful
   */
   int (*accel_cbc_decrypt)(const unsigned char *ct, unsigned char *pt, unsigned long blocks, unsigned char *IV, symmetric_key *skey);

   /** Accelerated CTR encryption
       @param pt      Plaintext
       @param ct      Ciphertext
       @param blocks  The number of complete blocks to process
       @param IV      The initial value (input/output)
       @param mode    little or big endian counter (mode=0 or mode=1)
       @param skey    The scheduled key context
       @return CRYPT_OK if successful
   */
   int (*accel_ctr_encrypt)(const unsigned char *pt, unsigned char *ct, unsigned long blocks, unsigned char *IV, int mode, symmetric_key *skey);

   /** Accelerated LRW
       @param pt      Plaintext
       @param ct      Ciphertext
       @param blocks  The number of complete blocks to process
       @param IV      The initial value (input/output)
       @param tweak   The LRW tweak
       @param skey    The scheduled key context
       @return CRYPT_OK if successful
   */
   int (*accel_lrw_encrypt)(const unsigned char *pt, unsigned char *ct, unsigned long blocks, unsigned char *IV, const unsigned char *tweak, symmetric_key *skey);

   /** Accelerated LRW
       @param ct      Ciphertext
       @param pt      Plaintext
       @param blocks  The number of complete blocks to process
       @param IV      The initial value (input/output)
       @param tweak   The LRW tweak
       @param skey    The scheduled key context
       @return CRYPT_OK if successful
   */
   int (*accel_lrw_decrypt)(const unsigned char *ct, unsigned char *pt, unsigned long blocks, unsigned char *IV, const unsigned char *tweak, symmetric_key *skey);

   /** Accelerated CCM packet (one-shot)
       @param key        The secret key to use
       @param keylen     The length of the secret key (octets)
       @param uskey      A previously scheduled key [optional can be NULL]
       @param nonce      The session nonce [use once]
       @param noncelen   The length of the nonce
       @param header     The header for the session
       @param headerlen  The length of the header (octets)
       @param pt         [out] The plaintext
       @param ptlen      The length of the plaintext (octets)
       @param ct         [out] The ciphertext
       @param tag        [out] The destination tag
       @param taglen     [in/out] The max size and resulting size of the authentication tag
       @param direction  Encrypt or Decrypt direction (0 or 1)
       @return CRYPT_OK if successful
   */
   int (*accel_ccm_memory)(
       const unsigned char *key,    unsigned long keylen,
       symmetric_key       *uskey,
       const unsigned char *nonce,  unsigned long noncelen,
       const unsigned char *header, unsigned long headerlen,
             unsigned char *pt,     unsigned long ptlen,
             unsigned char *ct,
             unsigned char *tag,    unsigned long *taglen,
                       int  direction);

   /** Accelerated GCM packet (one shot)
       @param key        The secret key
       @param keylen     The length of the secret key
       @param IV         The initialization vector
       @param IVlen      The length of the initialization vector
       @param adata      The additional authentication data (header)
       @param adatalen   The length of the adata
       @param pt         The plaintext
       @param ptlen      The length of the plaintext (ciphertext length is the same)
       @param ct         The ciphertext
       @param tag        [out] The MAC tag
       @param taglen     [in/out] The MAC tag length
       @param direction  Encrypt or Decrypt mode (GCM_ENCRYPT or GCM_DECRYPT)
       @return CRYPT_OK on success
   */
   int (*accel_gcm_memory)(
       const unsigned char *key,    unsigned long keylen,
       const unsigned char *IV,     unsigned long IVlen,
       const unsigned char *adata,  unsigned long adatalen,
             unsigned char *pt,     unsigned long ptlen,
             unsigned char *ct,
             unsigned char *tag,    unsigned long *taglen,
                       int direction);

   /** Accelerated one shot LTC_OMAC
       @param key            The secret key
       @param keylen         The key length (octets)
       @param in             The message
       @param inlen          Length of message (octets)
       @param out            [out] Destination for tag
       @param outlen         [in/out] Initial and final size of out
       @return CRYPT_OK on success
   */
   int (*omac_memory)(
       const unsigned char *key, unsigned long keylen,
       const unsigned char *in,  unsigned long inlen,
             unsigned char *out, unsigned long *outlen);

   /** Accelerated one shot XCBC
       @param key            The secret key
       @param keylen         The key length (octets)
       @param in             The message
       @param inlen          Length of message (octets)
       @param out            [out] Destination for tag
       @param outlen         [in/out] Initial and final size of out
       @return CRYPT_OK on success
   */
   int (*xcbc_memory)(
       const unsigned char *key, unsigned long keylen,
       const unsigned char *in,  unsigned long inlen,
             unsigned char *out, unsigned long *outlen);

   /** Accelerated one shot F9
       @param key            The secret key
       @param keylen         The key length (octets)
       @param in             The message
       @param inlen          Length of message (octets)
       @param out            [out] Destination for tag
       @param outlen         [in/out] Initial and final size of out
       @return CRYPT_OK on success
       @remark Requires manual padding
   */
   int (*f9_memory)(
       const unsigned char *key, unsigned long keylen,
       const unsigned char *in,  unsigned long inlen,
             unsigned char *out, unsigned long *outlen);

   /** Accelerated XTS encryption
       @param pt      Plaintext
       @param ct      Ciphertext
       @param blocks  The number of complete blocks to process
       @param tweak   The 128-bit encryption tweak (input/output).
                      The tweak should not be encrypted on input, but
                      next tweak will be copied encrypted on output.
       @param skey1   The first scheduled key context
       @param skey2   The second scheduled key context
       @return CRYPT_OK if successful
    */
    int (*accel_xts_encrypt)(const unsigned char *pt, unsigned char *ct,
        unsigned long blocks, unsigned char *tweak, symmetric_key *skey1,
        symmetric_key *skey2);

    /** Accelerated XTS decryption
        @param ct      Ciphertext
        @param pt      Plaintext
        @param blocks  The number of complete blocks to process
        @param tweak   The 128-bit encryption tweak (input/output).
                       The tweak should not be encrypted on input, but
                       next tweak will be copied encrypted on output.
        @param skey1   The first scheduled key context
        @param skey2   The second scheduled key context
        @return CRYPT_OK if successful
     */
     int (*accel_xts_decrypt)(const unsigned char *ct, unsigned char *pt,
         unsigned long blocks, unsigned char *tweak, symmetric_key *skey1,
         symmetric_key *skey2);
} cipher_descriptor[];

#ifdef LTC_BLOWFISH
int blowfish_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int blowfish_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int blowfish_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int blowfish_test(void);
void blowfish_done(symmetric_key *skey);
int blowfish_keysize(int *keysize);
extern const struct ltc_cipher_descriptor blowfish_desc;
#endif

#ifdef LTC_RC5
int rc5_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int rc5_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int rc5_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int rc5_test(void);
void rc5_done(symmetric_key *skey);
int rc5_keysize(int *keysize);
extern const struct ltc_cipher_descriptor rc5_desc;
#endif

#ifdef LTC_RC6
int rc6_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int rc6_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int rc6_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int rc6_test(void);
void rc6_done(symmetric_key *skey);
int rc6_keysize(int *keysize);
extern const struct ltc_cipher_descriptor rc6_desc;
#endif

#ifdef LTC_RC2
int rc2_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int rc2_setup_ex(const unsigned char *key, int keylen, int bits, int num_rounds, symmetric_key *skey);
int rc2_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int rc2_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int rc2_test(void);
void rc2_done(symmetric_key *skey);
int rc2_keysize(int *keysize);
extern const struct ltc_cipher_descriptor rc2_desc;
#endif

#ifdef LTC_SAFERP
int saferp_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int saferp_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int saferp_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int saferp_test(void);
void saferp_done(symmetric_key *skey);
int saferp_keysize(int *keysize);
extern const struct ltc_cipher_descriptor saferp_desc;
#endif

#ifdef LTC_SAFER
int safer_k64_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int safer_sk64_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int safer_k128_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int safer_sk128_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int safer_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *key);
int safer_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *key);
int safer_k64_test(void);
int safer_sk64_test(void);
int safer_sk128_test(void);
void safer_done(symmetric_key *skey);
int safer_64_keysize(int *keysize);
int safer_128_keysize(int *keysize);
extern const struct ltc_cipher_descriptor safer_k64_desc, safer_k128_desc, safer_sk64_desc, safer_sk128_desc;
#endif

#ifdef LTC_RIJNDAEL

/* make aes an alias */
#define aes_setup           rijndael_setup
#define aes_ecb_encrypt     rijndael_ecb_encrypt
#define aes_ecb_decrypt     rijndael_ecb_decrypt
#define aes_test            rijndael_test
#define aes_done            rijndael_done
#define aes_keysize         rijndael_keysize

#define aes_enc_setup           rijndael_enc_setup
#define aes_enc_ecb_encrypt     rijndael_enc_ecb_encrypt
#define aes_enc_keysize         rijndael_enc_keysize

int rijndael_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int rijndael_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int rijndael_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int rijndael_test(void);
void rijndael_done(symmetric_key *skey);
int rijndael_keysize(int *keysize);
int rijndael_enc_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int rijndael_enc_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
void rijndael_enc_done(symmetric_key *skey);
int rijndael_enc_keysize(int *keysize);
extern const struct ltc_cipher_descriptor rijndael_desc, aes_desc;
extern const struct ltc_cipher_descriptor rijndael_enc_desc, aes_enc_desc;
#endif

#ifdef LTC_XTEA
int xtea_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int xtea_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int xtea_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int xtea_test(void);
void xtea_done(symmetric_key *skey);
int xtea_keysize(int *keysize);
extern const struct ltc_cipher_descriptor xtea_desc;
#endif

#ifdef LTC_TWOFISH
int twofish_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int twofish_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int twofish_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int twofish_test(void);
void twofish_done(symmetric_key *skey);
int twofish_keysize(int *keysize);
extern const struct ltc_cipher_descriptor twofish_desc;
#endif

#ifdef LTC_DES
int des_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int des_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int des_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int des_test(void);
void des_done(symmetric_key *skey);
int des_keysize(int *keysize);
int des3_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int des3_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int des3_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int des3_test(void);
void des3_done(symmetric_key *skey);
int des3_keysize(int *keysize);
extern const struct ltc_cipher_descriptor des_desc, des3_desc;
#endif

#ifdef LTC_CAST5
int cast5_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int cast5_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int cast5_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int cast5_test(void);
void cast5_done(symmetric_key *skey);
int cast5_keysize(int *keysize);
extern const struct ltc_cipher_descriptor cast5_desc;
#endif

#ifdef LTC_NOEKEON
int noekeon_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int noekeon_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int noekeon_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int noekeon_test(void);
void noekeon_done(symmetric_key *skey);
int noekeon_keysize(int *keysize);
extern const struct ltc_cipher_descriptor noekeon_desc;
#endif

#ifdef LTC_SKIPJACK
int skipjack_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int skipjack_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int skipjack_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int skipjack_test(void);
void skipjack_done(symmetric_key *skey);
int skipjack_keysize(int *keysize);
extern const struct ltc_cipher_descriptor skipjack_desc;
#endif

#ifdef LTC_KHAZAD
int khazad_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int khazad_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int khazad_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int khazad_test(void);
void khazad_done(symmetric_key *skey);
int khazad_keysize(int *keysize);
extern const struct ltc_cipher_descriptor khazad_desc;
#endif

#ifdef LTC_ANUBIS
int anubis_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int anubis_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int anubis_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int anubis_test(void);
void anubis_done(symmetric_key *skey);
int anubis_keysize(int *keysize);
extern const struct ltc_cipher_descriptor anubis_desc;
#endif

#ifdef LTC_KSEED
int kseed_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int kseed_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int kseed_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int kseed_test(void);
void kseed_done(symmetric_key *skey);
int kseed_keysize(int *keysize);
extern const struct ltc_cipher_descriptor kseed_desc;
#endif

#ifdef LTC_KASUMI
int kasumi_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int kasumi_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int kasumi_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int kasumi_test(void);
void kasumi_done(symmetric_key *skey);
int kasumi_keysize(int *keysize);
extern const struct ltc_cipher_descriptor kasumi_desc;
#endif


#ifdef LTC_MULTI2
int multi2_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int multi2_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int multi2_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int multi2_test(void);
void multi2_done(symmetric_key *skey);
int multi2_keysize(int *keysize);
extern const struct ltc_cipher_descriptor multi2_desc;
#endif

#ifdef LTC_CAMELLIA
int camellia_setup(const unsigned char *key, int keylen, int num_rounds, symmetric_key *skey);
int camellia_ecb_encrypt(const unsigned char *pt, unsigned char *ct, symmetric_key *skey);
int camellia_ecb_decrypt(const unsigned char *ct, unsigned char *pt, symmetric_key *skey);
int camellia_test(void);
void camellia_done(symmetric_key *skey);
int camellia_keysize(int *keysize);
extern const struct ltc_cipher_descriptor camellia_desc;
#endif

#ifdef LTC_ECB_MODE
int ecb_start(int cipher, const unsigned char *key,
              int keylen, int num_rounds, symmetric_ECB *ecb);
int ecb_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_ECB *ecb);
int ecb_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_ECB *ecb);
int ecb_done(symmetric_ECB *ecb);
#endif

#ifdef LTC_CFB_MODE
int cfb_start(int cipher, const unsigned char *IV, const unsigned char *key,
              int keylen, int num_rounds, symmetric_CFB *cfb);
int cfb_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_CFB *cfb);
int cfb_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_CFB *cfb);
int cfb_getiv(unsigned char *IV, unsigned long *len, symmetric_CFB *cfb);
int cfb_setiv(const unsigned char *IV, unsigned long len, symmetric_CFB *cfb);
int cfb_done(symmetric_CFB *cfb);
#endif

#ifdef LTC_OFB_MODE
int ofb_start(int cipher, const unsigned char *IV, const unsigned char *key,
              int keylen, int num_rounds, symmetric_OFB *ofb);
int ofb_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_OFB *ofb);
int ofb_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_OFB *ofb);
int ofb_getiv(unsigned char *IV, unsigned long *len, symmetric_OFB *ofb);
int ofb_setiv(const unsigned char *IV, unsigned long len, symmetric_OFB *ofb);
int ofb_done(symmetric_OFB *ofb);
#endif

#ifdef LTC_CBC_MODE
int cbc_start(int cipher, const unsigned char *IV, const unsigned char *key,
               int keylen, int num_rounds, symmetric_CBC *cbc);
int cbc_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_CBC *cbc);
int cbc_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_CBC *cbc);
int cbc_getiv(unsigned char *IV, unsigned long *len, symmetric_CBC *cbc);
int cbc_setiv(const unsigned char *IV, unsigned long len, symmetric_CBC *cbc);
int cbc_done(symmetric_CBC *cbc);
#endif

#ifdef LTC_CTR_MODE

#define CTR_COUNTER_LITTLE_ENDIAN    0x0000
#define CTR_COUNTER_BIG_ENDIAN       0x1000
#define LTC_CTR_RFC3686              0x2000

int ctr_start(               int   cipher,
              const unsigned char *IV,
              const unsigned char *key,       int keylen,
                             int  num_rounds, int ctr_mode,
                   symmetric_CTR *ctr);
int ctr_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_CTR *ctr);
int ctr_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_CTR *ctr);
int ctr_getiv(unsigned char *IV, unsigned long *len, symmetric_CTR *ctr);
int ctr_setiv(const unsigned char *IV, unsigned long len, symmetric_CTR *ctr);
int ctr_done(symmetric_CTR *ctr);
int ctr_test(void);
#endif

#ifdef LTC_LRW_MODE

#define LRW_ENCRYPT LTC_ENCRYPT
#define LRW_DECRYPT LTC_DECRYPT

int lrw_start(               int   cipher,
              const unsigned char *IV,
              const unsigned char *key,       int keylen,
              const unsigned char *tweak,
                             int  num_rounds,
                   symmetric_LRW *lrw);
int lrw_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_LRW *lrw);
int lrw_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_LRW *lrw);
int lrw_getiv(unsigned char *IV, unsigned long *len, symmetric_LRW *lrw);
int lrw_setiv(const unsigned char *IV, unsigned long len, symmetric_LRW *lrw);
int lrw_done(symmetric_LRW *lrw);
int lrw_test(void);

/* don't call */
int lrw_process(const unsigned char *pt, unsigned char *ct, unsigned long len, int mode, symmetric_LRW *lrw);
#endif

#ifdef LTC_F8_MODE
int f8_start(                int  cipher, const unsigned char *IV,
             const unsigned char *key,                    int  keylen,
             const unsigned char *salt_key,               int  skeylen,
                             int  num_rounds,   symmetric_F8  *f8);
int f8_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_F8 *f8);
int f8_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_F8 *f8);
int f8_getiv(unsigned char *IV, unsigned long *len, symmetric_F8 *f8);
int f8_setiv(const unsigned char *IV, unsigned long len, symmetric_F8 *f8);
int f8_done(symmetric_F8 *f8);
int f8_test_mode(void);
#endif

#ifdef LTC_XTS_MODE
typedef struct {
   symmetric_key  key1, key2;
   int            cipher;
} symmetric_xts;

int xts_start(                int  cipher,
              const unsigned char *key1,
              const unsigned char *key2,
                    unsigned long  keylen,
                              int  num_rounds,
                    symmetric_xts *xts);

int xts_encrypt(
   const unsigned char *pt, unsigned long ptlen,
         unsigned char *ct,
         unsigned char *tweak,
         symmetric_xts *xts);
int xts_decrypt(
   const unsigned char *ct, unsigned long ptlen,
         unsigned char *pt,
         unsigned char *tweak,
         symmetric_xts *xts);

void xts_done(symmetric_xts *xts);
int  xts_test(void);
void xts_mult_x(unsigned char *I);
#endif

int find_cipher(const char *name);
int find_cipher_any(const char *name, int blocklen, int keylen);
int find_cipher_id(unsigned char ID);
int register_cipher(const struct ltc_cipher_descriptor *cipher);
int unregister_cipher(const struct ltc_cipher_descriptor *cipher);
int register_all_ciphers(void);
int cipher_is_valid(int idx);

LTC_MUTEX_PROTO(ltc_cipher_mutex)

/* ---- stream ciphers ---- */

#ifdef LTC_CHACHA

typedef struct {
   ulong32 input[16];
   unsigned char kstream[64];
   unsigned long ksleft;
   unsigned long ivlen;
   int rounds;
} chacha_state;

int chacha_setup(chacha_state *st, const unsigned char *key, unsigned long keylen, int rounds);
int chacha_ivctr32(chacha_state *st, const unsigned char *iv, unsigned long ivlen, ulong32 counter);
int chacha_ivctr64(chacha_state *st, const unsigned char *iv, unsigned long ivlen, ulong64 counter);
int chacha_crypt(chacha_state *st, const unsigned char *in, unsigned long inlen, unsigned char *out);
int chacha_keystream(chacha_state *st, unsigned char *out, unsigned long outlen);
int chacha_done(chacha_state *st);
int chacha_test(void);

#endif /* LTC_CHACHA */

#ifdef LTC_RC4_STREAM

typedef struct {
   unsigned int x, y;
   unsigned char buf[256];
} rc4_state;

int rc4_stream_setup(rc4_state *st, const unsigned char *key, unsigned long keylen);
int rc4_stream_crypt(rc4_state *st, const unsigned char *in, unsigned long inlen, unsigned char *out);
int rc4_stream_keystream(rc4_state *st, unsigned char *out, unsigned long outlen);
int rc4_stream_done(rc4_state *st);
int rc4_stream_test(void);

#endif /* LTC_RC4_STREAM */

#ifdef LTC_SOBER128_STREAM

typedef struct {
   ulong32 R[17],       /* Working storage for the shift register */
           initR[17],   /* saved register contents */
           konst,       /* key dependent constant */
           sbuf;        /* partial word encryption buffer */
   int     nbuf;        /* number of part-word stream bits buffered */
} sober128_state;

int sober128_stream_setup(sober128_state *st, const unsigned char *key, unsigned long keylen);
int sober128_stream_setiv(sober128_state *st, const unsigned char *iv, unsigned long ivlen);
int sober128_stream_crypt(sober128_state *st, const unsigned char *in, unsigned long inlen, unsigned char *out);
int sober128_stream_keystream(sober128_state *st, unsigned char *out, unsigned long outlen);
int sober128_stream_done(sober128_state *st);
int sober128_stream_test(void);

#endif /* LTC_SOBER128_STREAM */

/* ref:         HEAD -> master, tag: v1.18.2 */
/* git commit:  7e7eb695d581782f04b24dc444cbfde86af59853 */
/* commit time: 2018-07-01 22:49:01 +0200 */
