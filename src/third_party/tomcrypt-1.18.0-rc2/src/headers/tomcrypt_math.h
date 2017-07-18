/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

/** math functions **/

#define LTC_MP_LT   -1
#define LTC_MP_EQ    0
#define LTC_MP_GT    1

#define LTC_MP_NO    0
#define LTC_MP_YES   1

#ifndef LTC_MECC
   typedef void ecc_point;
#endif

#ifndef LTC_MRSA
   typedef void rsa_key;
#endif

#ifndef LTC_MILLER_RABIN_REPS
   /* Number of rounds of the Miller-Rabin test
    * "Reasonable values of reps are between 15 and 50." c.f. gmp doc of mpz_probab_prime_p() */
   #define LTC_MILLER_RABIN_REPS    35
#endif

int radix_to_bin(const void *in, int radix, void *out, size_t* len);

/** math descriptor */
typedef struct {
   /** Name of the math provider */
   char *name;

   /** Bits per digit, amount of bits must fit in an unsigned long */
   int  bits_per_digit;

/* ---- init/deinit functions ---- */

   /** initialize a bignum
     @param   a     The number to initialize
     @return  CRYPT_OK on success
   */
   int (*init)(void **a);

   /** init copy
     @param  dst    The number to initialize and write to
     @param  src    The number to copy from
     @return CRYPT_OK on success
   */
   int (*init_copy)(void **dst, void *src);

   /** deinit
      @param   a    The number to free
      @return CRYPT_OK on success
   */
   void (*deinit)(void *a);

/* ---- data movement ---- */

   /** negate
      @param   src   The number to negate
      @param   dst   The destination
      @return CRYPT_OK on success
   */
   int (*neg)(void *src, void *dst);

   /** copy
      @param   src   The number to copy from
      @param   dst   The number to write to
      @return CRYPT_OK on success
   */
   int (*copy)(void *src, void *dst);

/* ---- trivial low level functions ---- */

   /** set small constant
      @param a    Number to write to
      @param n    Source upto bits_per_digit (actually meant for very small constants)
      @return CRYPT_OK on success
   */
   int (*set_int)(void *a, ltc_mp_digit n);

   /** get small constant
      @param a  Small number to read,
                only fetches up to bits_per_digit from the number
      @return   The lower bits_per_digit of the integer (unsigned)
   */
   unsigned long (*get_int)(void *a);

   /** get digit n
     @param a  The number to read from
     @param n  The number of the digit to fetch
     @return  The bits_per_digit  sized n'th digit of a
   */
   ltc_mp_digit (*get_digit)(void *a, int n);

   /** Get the number of digits that represent the number
     @param a   The number to count
     @return The number of digits used to represent the number
   */
   int (*get_digit_count)(void *a);

   /** compare two integers
     @param a   The left side integer
     @param b   The right side integer
     @return LTC_MP_LT if a < b,
             LTC_MP_GT if a > b and
             LTC_MP_EQ otherwise.  (signed comparison)
   */
   int (*compare)(void *a, void *b);

   /** compare against int
     @param a   The left side integer
     @param b   The right side integer (upto bits_per_digit)
     @return LTC_MP_LT if a < b,
             LTC_MP_GT if a > b and
             LTC_MP_EQ otherwise.  (signed comparison)
   */
   int (*compare_d)(void *a, ltc_mp_digit n);

   /** Count the number of bits used to represent the integer
     @param a   The integer to count
     @return The number of bits required to represent the integer
   */
   int (*count_bits)(void * a);

   /** Count the number of LSB bits which are zero
     @param a   The integer to count
     @return The number of contiguous zero LSB bits
   */
   int (*count_lsb_bits)(void *a);

   /** Compute a power of two
     @param a  The integer to store the power in
     @param n  The power of two you want to store (a = 2^n)
     @return CRYPT_OK on success
   */
   int (*twoexpt)(void *a , int n);

/* ---- radix conversions ---- */

   /** read ascii string
     @param a     The integer to store into
     @param str   The string to read
     @param radix The radix the integer has been represented in (2-64)
     @return CRYPT_OK on success
   */
   int (*read_radix)(void *a, const char *str, int radix);

   /** write number to string
     @param a     The integer to store
     @param str   The destination for the string
     @param radix The radix the integer is to be represented in (2-64)
     @return CRYPT_OK on success
   */
   int (*write_radix)(void *a, char *str, int radix);

   /** get size as unsigned char string
     @param a  The integer to get the size (when stored in array of octets)
     @return   The length of the integer in octets
   */
   unsigned long (*unsigned_size)(void *a);

   /** store an integer as an array of octets
     @param src   The integer to store
     @param dst   The buffer to store the integer in
     @return CRYPT_OK on success
   */
   int (*unsigned_write)(void *src, unsigned char *dst);

   /** read an array of octets and store as integer
     @param dst   The integer to load
     @param src   The array of octets
     @param len   The number of octets
     @return CRYPT_OK on success
   */
   int (*unsigned_read)(         void *dst,
                        unsigned char *src,
                        unsigned long  len);

/* ---- basic math ---- */

   /** add two integers
     @param a   The first source integer
     @param b   The second source integer
     @param c   The destination of "a + b"
     @return CRYPT_OK on success
   */
   int (*add)(void *a, void *b, void *c);

   /** add two integers
     @param a   The first source integer
     @param b   The second source integer
                (single digit of upto bits_per_digit in length)
     @param c   The destination of "a + b"
     @return CRYPT_OK on success
   */
   int (*addi)(void *a, ltc_mp_digit b, void *c);

   /** subtract two integers
     @param a   The first source integer
     @param b   The second source integer
     @param c   The destination of "a - b"
     @return CRYPT_OK on success
   */
   int (*sub)(void *a, void *b, void *c);

   /** subtract two integers
     @param a   The first source integer
     @param b   The second source integer
                (single digit of upto bits_per_digit in length)
     @param c   The destination of "a - b"
     @return CRYPT_OK on success
   */
   int (*subi)(void *a, ltc_mp_digit b, void *c);

   /** multiply two integers
     @param a   The first source integer
     @param b   The second source integer
                (single digit of upto bits_per_digit in length)
     @param c   The destination of "a * b"
     @return CRYPT_OK on success
   */
   int (*mul)(void *a, void *b, void *c);

   /** multiply two integers
     @param a   The first source integer
     @param b   The second source integer
                (single digit of upto bits_per_digit in length)
     @param c   The destination of "a * b"
     @return CRYPT_OK on success
   */
   int (*muli)(void *a, ltc_mp_digit b, void *c);

   /** Square an integer
     @param a    The integer to square
     @param b    The destination
     @return CRYPT_OK on success
   */
   int (*sqr)(void *a, void *b);

   /** Divide an integer
     @param a    The dividend
     @param b    The divisor
     @param c    The quotient (can be NULL to signify don't care)
     @param d    The remainder (can be NULL to signify don't care)
     @return CRYPT_OK on success
   */
   int (*mpdiv)(void *a, void *b, void *c, void *d);

   /** divide by two
      @param  a   The integer to divide (shift right)
      @param  b   The destination
      @return CRYPT_OK on success
   */
   int (*div_2)(void *a, void *b);

   /** Get remainder (small value)
      @param  a    The integer to reduce
      @param  b    The modulus (upto bits_per_digit in length)
      @param  c    The destination for the residue
      @return CRYPT_OK on success
   */
   int (*modi)(void *a, ltc_mp_digit b, ltc_mp_digit *c);

   /** gcd
      @param  a     The first integer
      @param  b     The second integer
      @param  c     The destination for (a, b)
      @return CRYPT_OK on success
   */
   int (*gcd)(void *a, void *b, void *c);

   /** lcm
      @param  a     The first integer
      @param  b     The second integer
      @param  c     The destination for [a, b]
      @return CRYPT_OK on success
   */
   int (*lcm)(void *a, void *b, void *c);

   /** Modular multiplication
      @param  a     The first source
      @param  b     The second source
      @param  c     The modulus
      @param  d     The destination (a*b mod c)
      @return CRYPT_OK on success
   */
   int (*mulmod)(void *a, void *b, void *c, void *d);

   /** Modular squaring
      @param  a     The first source
      @param  b     The modulus
      @param  c     The destination (a*a mod b)
      @return CRYPT_OK on success
   */
   int (*sqrmod)(void *a, void *b, void *c);

   /** Modular inversion
      @param  a     The value to invert
      @param  b     The modulus
      @param  c     The destination (1/a mod b)
      @return CRYPT_OK on success
   */
   int (*invmod)(void *, void *, void *);

/* ---- reduction ---- */

   /** setup Montgomery
       @param a  The modulus
       @param b  The destination for the reduction digit
       @return CRYPT_OK on success
   */
   int (*montgomery_setup)(void *a, void **b);

   /** get normalization value
       @param a   The destination for the normalization value
       @param b   The modulus
       @return  CRYPT_OK on success
   */
   int (*montgomery_normalization)(void *a, void *b);

   /** reduce a number
       @param a   The number [and dest] to reduce
       @param b   The modulus
       @param c   The value "b" from montgomery_setup()
       @return CRYPT_OK on success
   */
   int (*montgomery_reduce)(void *a, void *b, void *c);

   /** clean up  (frees memory)
       @param a   The value "b" from montgomery_setup()
       @return CRYPT_OK on success
   */
   void (*montgomery_deinit)(void *a);

/* ---- exponentiation ---- */

   /** Modular exponentiation
       @param a    The base integer
       @param b    The power (can be negative) integer
       @param c    The modulus integer
       @param d    The destination
       @return CRYPT_OK on success
   */
   int (*exptmod)(void *a, void *b, void *c, void *d);

   /** Primality testing
       @param a     The integer to test
       @param b     The number of Miller-Rabin tests that shall be executed
       @param c     The destination of the result (FP_YES if prime)
       @return CRYPT_OK on success
   */
   int (*isprime)(void *a, int b, int *c);

/* ----  (optional) ecc point math ---- */

   /** ECC GF(p) point multiplication (from the NIST curves)
       @param k   The integer to multiply the point by
       @param G   The point to multiply
       @param R   The destination for kG
       @param modulus  The modulus for the field
       @param map Boolean indicated whether to map back to affine or not
                  (can be ignored if you work in affine only)
       @return CRYPT_OK on success
   */
   int (*ecc_ptmul)(     void *k,
                    ecc_point *G,
                    ecc_point *R,
                         void *modulus,
                          int  map);

   /** ECC GF(p) point addition
       @param P    The first point
       @param Q    The second point
       @param R    The destination of P + Q
       @param modulus  The modulus
       @param mp   The "b" value from montgomery_setup()
       @return CRYPT_OK on success
   */
   int (*ecc_ptadd)(ecc_point *P,
                    ecc_point *Q,
                    ecc_point *R,
                         void *modulus,
                         void *mp);

   /** ECC GF(p) point double
       @param P    The first point
       @param R    The destination of 2P
       @param modulus  The modulus
       @param mp   The "b" value from montgomery_setup()
       @return CRYPT_OK on success
   */
   int (*ecc_ptdbl)(ecc_point *P,
                    ecc_point *R,
                         void *modulus,
                         void *mp);

   /** ECC mapping from projective to affine,
       currently uses (x,y,z) => (x/z^2, y/z^3, 1)
       @param P     The point to map
       @param modulus The modulus
       @param mp    The "b" value from montgomery_setup()
       @return CRYPT_OK on success
       @remark The mapping can be different but keep in mind a
               ecc_point only has three integers (x,y,z) so if
               you use a different mapping you have to make it fit.
   */
   int (*ecc_map)(ecc_point *P, void *modulus, void *mp);

   /** Computes kA*A + kB*B = C using Shamir's Trick
       @param A        First point to multiply
       @param kA       What to multiple A by
       @param B        Second point to multiply
       @param kB       What to multiple B by
       @param C        [out] Destination point (can overlap with A or B)
       @param modulus  Modulus for curve
       @return CRYPT_OK on success
   */
   int (*ecc_mul2add)(ecc_point *A, void *kA,
                      ecc_point *B, void *kB,
                      ecc_point *C,
                           void *modulus);

/* ---- (optional) rsa optimized math (for internal CRT) ---- */

   /** RSA Key Generation
       @param prng     An active PRNG state
       @param wprng    The index of the PRNG desired
       @param size     The size of the key in octets
       @param e        The "e" value (public key).
                       e==65537 is a good choice
       @param key      [out] Destination of a newly created private key pair
       @return CRYPT_OK if successful, upon error all allocated ram is freed
    */
    int (*rsa_keygen)(prng_state *prng,
                             int  wprng,
                             int  size,
                            long  e,
                         rsa_key *key);

   /** RSA exponentiation
      @param in       The octet array representing the base
      @param inlen    The length of the input
      @param out      The destination (to be stored in an octet array format)
      @param outlen   The length of the output buffer and the resulting size
                      (zero padded to the size of the modulus)
      @param which    PK_PUBLIC for public RSA and PK_PRIVATE for private RSA
      @param key      The RSA key to use
      @return CRYPT_OK on success
   */
   int (*rsa_me)(const unsigned char *in,   unsigned long inlen,
                       unsigned char *out,  unsigned long *outlen, int which,
                       rsa_key *key);

/* ---- basic math continued ---- */

   /** Modular addition
      @param  a     The first source
      @param  b     The second source
      @param  c     The modulus
      @param  d     The destination (a + b mod c)
      @return CRYPT_OK on success
   */
   int (*addmod)(void *a, void *b, void *c, void *d);

   /** Modular substraction
      @param  a     The first source
      @param  b     The second source
      @param  c     The modulus
      @param  d     The destination (a - b mod c)
      @return CRYPT_OK on success
   */
   int (*submod)(void *a, void *b, void *c, void *d);

/* ---- misc stuff ---- */

   /** Make a pseudo-random mpi
      @param  a     The mpi to make random
      @param  size  The desired length
      @return CRYPT_OK on success
   */
   int (*rand)(void *a, int size);
} ltc_math_descriptor;

extern ltc_math_descriptor ltc_mp;

int ltc_init_multi(void **a, ...);
void ltc_deinit_multi(void *a, ...);
void ltc_cleanup_multi(void **a, ...);

#ifdef LTM_DESC
extern const ltc_math_descriptor ltm_desc;
#endif

#ifdef TFM_DESC
extern const ltc_math_descriptor tfm_desc;
#endif

#ifdef GMP_DESC
extern const ltc_math_descriptor gmp_desc;
#endif

#if !defined(DESC_DEF_ONLY) && defined(LTC_SOURCE)

#define MP_DIGIT_BIT                 ltc_mp.bits_per_digit

/* some handy macros */
#define mp_init(a)                   ltc_mp.init(a)
#define mp_init_multi                ltc_init_multi
#define mp_clear(a)                  ltc_mp.deinit(a)
#define mp_clear_multi               ltc_deinit_multi
#define mp_cleanup_multi             ltc_cleanup_multi
#define mp_init_copy(a, b)           ltc_mp.init_copy(a, b)

#define mp_neg(a, b)                 ltc_mp.neg(a, b)
#define mp_copy(a, b)                ltc_mp.copy(a, b)

#define mp_set(a, b)                 ltc_mp.set_int(a, b)
#define mp_set_int(a, b)             ltc_mp.set_int(a, b)
#define mp_get_int(a)                ltc_mp.get_int(a)
#define mp_get_digit(a, n)           ltc_mp.get_digit(a, n)
#define mp_get_digit_count(a)        ltc_mp.get_digit_count(a)
#define mp_cmp(a, b)                 ltc_mp.compare(a, b)
#define mp_cmp_d(a, b)               ltc_mp.compare_d(a, b)
#define mp_count_bits(a)             ltc_mp.count_bits(a)
#define mp_cnt_lsb(a)                ltc_mp.count_lsb_bits(a)
#define mp_2expt(a, b)               ltc_mp.twoexpt(a, b)

#define mp_read_radix(a, b, c)       ltc_mp.read_radix(a, b, c)
#define mp_toradix(a, b, c)          ltc_mp.write_radix(a, b, c)
#define mp_unsigned_bin_size(a)      ltc_mp.unsigned_size(a)
#define mp_to_unsigned_bin(a, b)     ltc_mp.unsigned_write(a, b)
#define mp_read_unsigned_bin(a, b, c) ltc_mp.unsigned_read(a, b, c)

#define mp_add(a, b, c)              ltc_mp.add(a, b, c)
#define mp_add_d(a, b, c)            ltc_mp.addi(a, b, c)
#define mp_sub(a, b, c)              ltc_mp.sub(a, b, c)
#define mp_sub_d(a, b, c)            ltc_mp.subi(a, b, c)
#define mp_mul(a, b, c)              ltc_mp.mul(a, b, c)
#define mp_mul_d(a, b, c)            ltc_mp.muli(a, b, c)
#define mp_sqr(a, b)                 ltc_mp.sqr(a, b)
#define mp_div(a, b, c, d)           ltc_mp.mpdiv(a, b, c, d)
#define mp_div_2(a, b)               ltc_mp.div_2(a, b)
#define mp_mod(a, b, c)              ltc_mp.mpdiv(a, b, NULL, c)
#define mp_mod_d(a, b, c)            ltc_mp.modi(a, b, c)
#define mp_gcd(a, b, c)              ltc_mp.gcd(a, b, c)
#define mp_lcm(a, b, c)              ltc_mp.lcm(a, b, c)

#define mp_addmod(a, b, c, d)        ltc_mp.addmod(a, b, c, d)
#define mp_submod(a, b, c, d)        ltc_mp.submod(a, b, c, d)
#define mp_mulmod(a, b, c, d)        ltc_mp.mulmod(a, b, c, d)
#define mp_sqrmod(a, b, c)           ltc_mp.sqrmod(a, b, c)
#define mp_invmod(a, b, c)           ltc_mp.invmod(a, b, c)

#define mp_montgomery_setup(a, b)    ltc_mp.montgomery_setup(a, b)
#define mp_montgomery_normalization(a, b) ltc_mp.montgomery_normalization(a, b)
#define mp_montgomery_reduce(a, b, c)   ltc_mp.montgomery_reduce(a, b, c)
#define mp_montgomery_free(a)        ltc_mp.montgomery_deinit(a)

#define mp_exptmod(a,b,c,d)          ltc_mp.exptmod(a,b,c,d)
#define mp_prime_is_prime(a, b, c)   ltc_mp.isprime(a, b, c)

#define mp_iszero(a)                 (mp_cmp_d(a, 0) == LTC_MP_EQ ? LTC_MP_YES : LTC_MP_NO)
#define mp_isodd(a)                  (mp_get_digit_count(a) > 0 ? (mp_get_digit(a, 0) & 1 ? LTC_MP_YES : LTC_MP_NO) : LTC_MP_NO)
#define mp_exch(a, b)                do { void *ABC__tmp = a; a = b; b = ABC__tmp; } while(0)

#define mp_tohex(a, b)               mp_toradix(a, b, 16)

#define mp_rand(a, b)                ltc_mp.rand(a, b)

#endif

/* ref:         HEAD -> release/1.18.0, tag: v1.18.0-rc2 */
/* git commit:  aa0f396c0c8828ce39456129507fc72ef0208bd0 */
/* commit time: 2017-07-13 14:58:01 +0200 */
