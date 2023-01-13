#include "./int128.h"

#include <iostream>
#include <random>
#include <thread>
#include <string>
#include <vector>

#if (defined(__GNUC__) && __GNUC__ < 7 && !defined(__clang__)) || \
   (defined(_MSC_VER) && _MSC_VER < 1920)
// Old GCC and old MSVC have partially-broken constexpr that prevents us from
// properly using static_assert with from_string()
#define BROKEN_CONSTEXPR
#endif

#ifndef BROKEN_CONSTEXPR
// Basic checks with static_asserts, check constexpr correctness and fail fast
static_assert (mlib_int128_eq (MLIB_INT128 (0), MLIB_INT128_FROM_PARTS (0, 0)),
               "fail");
static_assert (mlib_int128_eq (MLIB_INT128 (4), MLIB_INT128_FROM_PARTS (4, 0)),
               "fail");
static_assert (mlib_int128_eq (MLIB_INT128 (34),
                               MLIB_INT128_FROM_PARTS (34, 0)),
               "fail");
static_assert (mlib_int128_eq (MLIB_INT128 (34 + 8),
                               MLIB_INT128_FROM_PARTS (42, 0)),
               "fail");
static_assert (mlib_int128_eq (MLIB_INT128_CAST (94),
                               MLIB_INT128_FROM_PARTS (94, 0)),
               "fail");
static_assert (mlib_int128_eq (mlib_int128_lshift (MLIB_INT128_CAST (1), 64),
                               MLIB_INT128_FROM_PARTS (0, 1)),
               "fail");
static_assert (mlib_int128_eq (mlib_int128_lshift (MLIB_INT128_CAST (1), 127),
                               MLIB_INT128_FROM_PARTS (0, 1ull << 63)),
               "fail");

static_assert (mlib_int128_scmp (MLIB_INT128_CAST (2), MLIB_INT128 (0)) > 0,
               "fail");
static_assert (mlib_int128_scmp (MLIB_INT128_CAST (-2), MLIB_INT128 (0)) < 0,
               "fail");
static_assert (mlib_int128_scmp (MLIB_INT128_CAST (0), MLIB_INT128 (0)) == 0,
               "fail");
// Unsigned compare doesn't believe in negative numbers:
static_assert (mlib_int128_ucmp (MLIB_INT128_CAST (-2), MLIB_INT128 (0)) > 0,
               "fail");
#endif // BROKEN_CONSTEXPR

// Literals, for test convenience:
#ifndef BROKEN_CONSTEXPR
constexpr
#endif
   mlib_int128
   operator""_i128 (const char *s)
{
   return mlib_int128_from_string (s, NULL);
}

#ifndef BROKEN_CONSTEXPR
constexpr
#endif
   mlib_int128
   operator""_i128 (const char *s, size_t)
{
   return mlib_int128_from_string (s, NULL);
}

// Operators, for test convenience
constexpr bool
operator== (mlib_int128 l, mlib_int128 r)
{
   return mlib_int128_eq (l, r);
}

constexpr bool
operator<(mlib_int128 l, mlib_int128 r)
{
   return mlib_int128_scmp (l, r) < 0;
}

#ifndef BROKEN_CONSTEXPR
static_assert (mlib_int128_eq (MLIB_INT128 (0), 0_i128), "fail");
static_assert (mlib_int128_eq (MLIB_INT128 (65025), 65025_i128), "fail");
static_assert (mlib_int128_eq (MLIB_INT128_FROM_PARTS (0, 1),
                               18446744073709551616_i128),
               "fail");
static_assert (mlib_int128_eq (MLIB_INT128_UMAX,
                               340282366920938463463374607431768211455_i128),
               "fail");

static_assert (mlib_int128_scmp (MLIB_INT128_SMIN, MLIB_INT128_SMAX) < 0,
               "fail");
static_assert (mlib_int128_scmp (MLIB_INT128_SMAX, MLIB_INT128_SMIN) > 0,
               "fail");
static_assert (mlib_int128_scmp (MLIB_INT128_CAST (-12), MLIB_INT128_CAST (0)) <
                  0,
               "fail");
static_assert (mlib_int128_scmp (MLIB_INT128_CAST (12), MLIB_INT128_CAST (0)) >
                  0,
               "fail");

// Simple arithmetic:
static_assert (mlib_int128_scmp (mlib_int128_add (MLIB_INT128_SMAX, 1_i128),
                                 MLIB_INT128_SMIN) == 0,
               "fail");
static_assert (mlib_int128_scmp (mlib_int128_negate (MLIB_INT128_CAST (-42)),
                                 MLIB_INT128 (42)) == 0,
               "fail");
static_assert (mlib_int128_scmp (mlib_int128_sub (5_i128, 3_i128), 2_i128) == 0,
               "fail");
static_assert (mlib_int128_scmp (mlib_int128_sub (3_i128, 5_i128),
                                 mlib_int128_negate (2_i128)) == 0,
               "fail");
static_assert (mlib_int128_ucmp (mlib_int128_sub (3_i128, 5_i128),
                                 mlib_int128_sub (MLIB_INT128_UMAX, 1_i128)) ==
                  0,
               "fail");

static_assert (mlib_int128_scmp (mlib_int128_lshift (1_i128, 127),
                                 MLIB_INT128_SMIN) == 0,
               "fail");

static_assert (
   mlib_int128_scmp (mlib_int128_rshift (mlib_int128_lshift (1_i128, 127), 127),
                     1_i128) == 0,
   "fail");

// With no high-32 bits in the denominator
static_assert (mlib_int128_div (316356263640858117670580590964547584140_i128,
                                13463362962560749016052695684_i128) ==
                  23497566285_i128,
               "fail");

// Remainder correctness with high bit set:
static_assert (mlib_int128_mod (292590981272581782572061492191999425232_i128,
                                221673222198185508195462959065350495048_i128) ==
                  70917759074396274376598533126648930184_i128,
               "fail");

// Remainder with 64bit denom:
static_assert (mlib_int128_mod (2795722437127403543495742528_i128,
                                708945413_i128) == 619266642_i128,
               "fail");

// 10-div:
static_assert (mlib_int128_div (MLIB_INT128_SMAX, 10_i128) ==
                  17014118346046923173168730371588410572_i128,
               "fail");
#endif // BROKEN_CONSTEXPR

inline std::ostream &
operator<< (std::ostream &out, const mlib_int128 &v)
{
   out << mlib_int128_format (v).str;
   return out;
}

struct check_info {
   const char *filename;
   int line;
   const char *expr;
};

struct nil {
};

template <typename Left> struct bound_lhs {
   check_info info;
   Left value;

#define DEFOP(Oper)                                                   \
   template <typename Rhs> nil operator Oper (Rhs rhs) const noexcept \
   {                                                                  \
      if (value Oper rhs) {                                           \
         return {};                                                   \
      }                                                               \
      fprintf (stderr,                                                \
               "%s:%d: CHECK( %s ) failed!\n",                        \
               info.filename,                                         \
               info.line,                                             \
               info.expr);                                            \
      fprintf (stderr, "Expanded expression: ");                      \
      std::cerr << value << " " #Oper " " << rhs << '\n';             \
      std::exit (1);                                                  \
      return {};                                                      \
   }
   DEFOP (==)
   DEFOP (!=)
   DEFOP (<)
   DEFOP (<=)
   DEFOP (>)
   DEFOP (>=)
#undef DEFOP
};

struct check_magic {
   check_info info;

   template <typename Oper>
   bound_lhs<Oper>
   operator->*(Oper op)
   {
      return bound_lhs<Oper>{info, op};
   }
};

struct check_consume {
   void
   operator= (nil)
   {
   }

   void
   operator= (bound_lhs<bool> const &l)
   {
      // Invoke the test for truthiness:
      (void) (l == true);
   }
};

#undef CHECK
#define CHECK(Cond) \
   check_consume{} = check_magic{check_info{__FILE__, __LINE__, #Cond}}->*Cond

#ifndef BROKEN_CONSTEXPR
static_assert (mlib_int128 (MLIB_INT128_UMAX) ==
                  340282366920938463463374607431768211455_i128,
               "fail");

// Check sign extension works correctly:
static_assert (mlib_int128 (MLIB_INT128_CAST (INT64_MIN)) ==
                  mlib_int128_negate (9223372036854775808_i128),
               "fail");
static_assert (mlib_int128 (MLIB_INT128_CAST (INT64_MIN)) <
                  mlib_int128_negate (9223372036854775807_i128),
               "fail");
static_assert (mlib_int128_negate (9223372036854775809_i128) <
                  mlib_int128 (MLIB_INT128_CAST (INT64_MIN)),
               "fail");
#endif

static mlib_int128_divmod_result
div_check (mlib_int128 num, mlib_int128 den)
{
   // std::cout << "Check: " << num << " ÷ " << den << '\n';
   mlib_int128_divmod_result res = mlib_int128_divmod (num, den);
#ifdef __SIZEOF_INT128__
   // When we have an existing i128 impl, test against that:
   __uint128_t num1;
   __uint128_t den1;
   memcpy (&num1, &num.r, sizeof num);
   memcpy (&den1, &den.r, sizeof den);
   __uint128_t q = num1 / den1;
   __uint128_t r = num1 % den1;
   mlib_int128_divmod_result expect;
   memcpy (&expect.quotient.r, &q, sizeof q);
   memcpy (&expect.remainder.r, &r, sizeof r);
   CHECK (expect.quotient == res.quotient);
   CHECK (expect.remainder == res.remainder);
#endif
   // Check inversion by multiplication provides the correct result
   auto invert = mlib_int128_mul (res.quotient, den);
   invert = mlib_int128_add (invert, res.remainder);
   CHECK (invert == num);
   return res;
}

// Runtime checks, easier to debug that static_asserts
int
main ()
{
   mlib_int128 zero = MLIB_INT128 (0);
   CHECK (mlib_int128_eq (zero, MLIB_INT128 (0)));
   CHECK (mlib_int128_eq (zero, 0_i128));
   CHECK (zero == 0_i128);

   auto two = MLIB_INT128 (2);
   auto four = mlib_int128_add (two, two);
   CHECK (four == MLIB_INT128 (4));
   CHECK (four == 4_i128);
   CHECK (two == mlib_int128_add (two, zero));

   // Addition wraps:
   mlib_int128 max = MLIB_INT128_SMAX;
   auto more = mlib_int128_add (max, four);
   CHECK (more == mlib_int128_add (MLIB_INT128_SMIN, MLIB_INT128 (3)));

   // "Wrap" around zero:
   auto ntwo = MLIB_INT128_CAST (-2);
   auto sum = mlib_int128_add (ntwo, four);
   CHECK (sum == two);

   auto eight = mlib_int128_lshift (two, 2);
   CHECK (eight == MLIB_INT128 (8));

   auto big = mlib_int128_lshift (two, 72);
   CHECK (mlib_int128_scmp (big, MLIB_INT128 (0)) > 0);

   auto four_v2 = mlib_int128_lshift (eight, -1);
   CHECK (four == four_v2);

   // Negative literals:
   CHECK (MLIB_INT128 (-64) == mlib_int128_negate (64_i128));

   CHECK (mlib_int128_mul (1_i128, 2_i128) == 2_i128);
   CHECK (mlib_int128_mul (1_i128, 0_i128) == 0_i128);
   CHECK (mlib_int128_mul (0_i128, 0_i128) == 0_i128);
   CHECK (mlib_int128_mul (2_i128, 73_i128) == 146_i128);
   CHECK (mlib_int128_mul (28468554863115876158655557_i128, 73_i128) ==
          2078204505007458959581855661_i128);
   CHECK (mlib_int128_mul (MLIB_INT128_CAST (-7), 4_i128) ==
          MLIB_INT128_CAST (-28));
   CHECK (mlib_int128_mul (MLIB_INT128_CAST (-7), MLIB_INT128_CAST (-7)) ==
          49_i128);

   // It's useful to specify bit patterns directly
   auto in_binary =
      0b110101010110100100001101111001111010100010111100100101101011010110101001010110110011000100000100011110010101101001111110001000_i128;
   CHECK (in_binary == 70917759074396274376598533126648930184_i128);
   CHECK (
      in_binary ==
      "0b110101010110100100001101111001111010100010111100100101101011010110101001010110110011000100000100011110010101101001111110001000"_i128);

   // Or hexadecimal
   auto in_hex = 0x355a4379ea2f25ad6a56cc411e569f88_i128;
   CHECK (in_hex == 70917759074396274376598533126648930184_i128);

   int8_t n = -12;
   CHECK (mlib_int128_scmp (zero, MLIB_INT128_CAST (n)) > 0);
   CHECK (mlib_int128_ucmp (zero, MLIB_INT128_CAST (n)) < 0);

   auto _2pow127 = mlib_int128_pow2 (127);
   CHECK (std::string (mlib_int128_format (_2pow127).str) ==
          "170141183460469231731687303715884105728");

   auto r = div_check (27828649044156246570177174673037165454_i128,
                       499242349997913298655486252455941907_i128);

   CHECK (r.quotient == 55_i128);
   CHECK (r.remainder == 370319794271015144125430787960360569_i128);

   r = div_check (64208687961221311123721027584_i128, 3322092839076102144_i128);
   CHECK (r.remainder == 3155565729965670400_i128);

   // This division will trigger the rare Knuth 4.3.1D/D6 condition:
   r = div_check (31322872034807296605612234499929458960_i128,
                  34573864092216774938021667884_i128);
   CHECK (r.quotient == 905969663_i128);
   CHECK (r.remainder == 34573864092065898160364055868_i128);

   // Self-divide:
   r = div_check (628698094597401606590302208_i128,
                  628698094597401606590302208_i128);
   CHECK (r.quotient == 1_i128);
   CHECK (r.remainder == 0_i128);

   // With no high-32 bits in the denominator
   r = div_check (316356263640858117670580590964547584140_i128,
                  13463362962560749016052695684_i128);
   CHECK (r.quotient == 23497566285_i128);

   // Remainder correctness with high bit set:
   r = div_check (292590981272581782572061492191999425232_i128,
                  221673222198185508195462959065350495048_i128);
   CHECK (r.remainder == 70917759074396274376598533126648930184_i128);

   // Remainder with 64bit denom:
   r = div_check (2795722437127403543495742528_i128, 708945413_i128);
   CHECK (r.remainder == 619266642_i128);

   // 10-div:
   CHECK (mlib_int128_div (MLIB_INT128_SMAX, 10_i128) ==
          17014118346046923173168730371588410572_i128);

   std::random_device rd;
   std::seed_seq seed ({rd (), rd (), rd (), rd ()});
   // Pick every numerator bit pattern from 0b00'00 to 0b11'11
   for (auto nbits = 0u; nbits < 16u; ++nbits) {
      // This is an extremely rudimentary thread pool to parallelize the
      // division checks. It doesn't need to be rigorous or optimal, it only
      // needs to "just work."
      std::vector<std::thread> threads;
      // Pick every denominator bit pattern from 0b00'01 to 0b11'11:
      for (auto dbits = 1u; dbits < 16u; ++dbits) {
         // Randomness:
         std::mt19937 random;
         random.seed (seed);
         // Spawn a thread for this denominator bit pattern:
         threads.emplace_back ([nbits, dbits, random] () mutable {
            std::uniform_int_distribution<std::uint32_t> dist;
            // 100k random divisions:
            for (auto i = 0; i < 100000; ++i) {
               // Generate a denominator
               auto den = 0_i128;
               while (den == 0_i128) {
                  // Regenerate until we don't have zero (very
                  // unlikely, but be safe!)
                  uint64_t dlo = 0, dhi = 0;
                  (dbits & 1) && (dlo |= dist (random));
                  (dbits & 2) && (dlo |= (uint64_t) dist (random) << 32);
                  (dbits & 4) && (dhi |= dist (random));
                  (dbits & 8) && (dhi |= (uint64_t) dist (random) << 32);
                  den = MLIB_INT128_FROM_PARTS (dlo, dhi);
               }
               // Generate a numerator
               uint64_t nlo = 0, nhi = 0;
               (nbits & 1) && (nlo |= dist (random));
               (nbits & 2) && (nlo |= (uint64_t) dist (random) << 32);
               (nbits & 4) && (nhi |= dist (random));
               (nbits & 8) && (nhi |= (uint64_t) dist (random) << 32);
               mlib_int128 num = MLIB_INT128_FROM_PARTS (nlo, nhi);
               // Divide them:
               div_check (num, den);
            }
         });
      }
      // Join the threads that are dividing:
      for (auto &t : threads) {
         t.join ();
      }
   }
}
