#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <cmath>
#include <string>

#include "gtest/gtest.h"
#include "absl/strings/internal/str_format/bind.h"

namespace absl {
namespace str_format_internal {
namespace {

template <typename T, size_t N>
size_t ArraySize(T (&)[N]) {
  return N;
}

std::string LengthModFor(float) { return ""; }
std::string LengthModFor(double) { return ""; }
std::string LengthModFor(long double) { return "L"; }
std::string LengthModFor(char) { return "hh"; }
std::string LengthModFor(signed char) { return "hh"; }
std::string LengthModFor(unsigned char) { return "hh"; }
std::string LengthModFor(short) { return "h"; }           // NOLINT
std::string LengthModFor(unsigned short) { return "h"; }  // NOLINT
std::string LengthModFor(int) { return ""; }
std::string LengthModFor(unsigned) { return ""; }
std::string LengthModFor(long) { return "l"; }                 // NOLINT
std::string LengthModFor(unsigned long) { return "l"; }        // NOLINT
std::string LengthModFor(long long) { return "ll"; }           // NOLINT
std::string LengthModFor(unsigned long long) { return "ll"; }  // NOLINT

std::string EscCharImpl(int v) {
  if (isprint(v)) return std::string(1, static_cast<char>(v));
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "\\%#.2x",
                   static_cast<unsigned>(v & 0xff));
  assert(n > 0 && n < sizeof(buf));
  return std::string(buf, n);
}

std::string Esc(char v) { return EscCharImpl(v); }
std::string Esc(signed char v) { return EscCharImpl(v); }
std::string Esc(unsigned char v) { return EscCharImpl(v); }

template <typename T>
std::string Esc(const T &v) {
  std::ostringstream oss;
  oss << v;
  return oss.str();
}

void StrAppend(std::string *dst, const char *format, va_list ap) {
  // First try with a small fixed size buffer
  static const int kSpaceLength = 1024;
  char space[kSpaceLength];

  // It's possible for methods that use a va_list to invalidate
  // the data in it upon use.  The fix is to make a copy
  // of the structure before using it and use that copy instead.
  va_list backup_ap;
  va_copy(backup_ap, ap);
  int result = vsnprintf(space, kSpaceLength, format, backup_ap);
  va_end(backup_ap);
  if (result < kSpaceLength) {
    if (result >= 0) {
      // Normal case -- everything fit.
      dst->append(space, result);
      return;
    }
    if (result < 0) {
      // Just an error.
      return;
    }
  }

  // Increase the buffer size to the size requested by vsnprintf,
  // plus one for the closing \0.
  int length = result + 1;
  char *buf = new char[length];

  // Restore the va_list before we use it again
  va_copy(backup_ap, ap);
  result = vsnprintf(buf, length, format, backup_ap);
  va_end(backup_ap);

  if (result >= 0 && result < length) {
    // It fit
    dst->append(buf, result);
  }
  delete[] buf;
}

std::string StrPrint(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  std::string result;
  StrAppend(&result, format, ap);
  va_end(ap);
  return result;
}

class FormatConvertTest : public ::testing::Test { };

template <typename T>
void TestStringConvert(const T& str) {
  const FormatArgImpl args[] = {FormatArgImpl(str)};
  struct Expectation {
    const char *out;
    const char *fmt;
  };
  const Expectation kExpect[] = {
    {"hello",  "%1$s"      },
    {"",       "%1$.s"     },
    {"",       "%1$.0s"    },
    {"h",      "%1$.1s"    },
    {"he",     "%1$.2s"    },
    {"hello",  "%1$.10s"   },
    {" hello", "%1$6s"     },
    {"   he",  "%1$5.2s"   },
    {"he   ",  "%1$-5.2s"  },
    {"hello ", "%1$-6.10s" },
  };
  for (const Expectation &e : kExpect) {
    UntypedFormatSpecImpl format(e.fmt);
    EXPECT_EQ(e.out, FormatPack(format, absl::MakeSpan(args)));
  }
}

TEST_F(FormatConvertTest, BasicString) {
  TestStringConvert("hello");  // As char array.
  TestStringConvert(static_cast<const char*>("hello"));
  TestStringConvert(std::string("hello"));
  TestStringConvert(string_view("hello"));
}

TEST_F(FormatConvertTest, NullString) {
  const char* p = nullptr;
  UntypedFormatSpecImpl format("%s");
  EXPECT_EQ("", FormatPack(format, {FormatArgImpl(p)}));
}

TEST_F(FormatConvertTest, StringPrecision) {
  // We cap at the precision.
  char c = 'a';
  const char* p = &c;
  UntypedFormatSpecImpl format("%.1s");
  EXPECT_EQ("a", FormatPack(format, {FormatArgImpl(p)}));

  // We cap at the nul terminator.
  p = "ABC";
  UntypedFormatSpecImpl format2("%.10s");
  EXPECT_EQ("ABC", FormatPack(format2, {FormatArgImpl(p)}));
}

TEST_F(FormatConvertTest, Pointer) {
#if _MSC_VER
  // MSVC's printf implementation prints pointers differently. We can't easily
  // compare our implementation to theirs.
  return;
#endif
  static int x = 0;
  const int *xp = &x;
  char c = 'h';
  char *mcp = &c;
  const char *cp = "hi";
  const char *cnil = nullptr;
  const int *inil = nullptr;
  using VoidF = void (*)();
  VoidF fp = [] {}, fnil = nullptr;
  volatile char vc;
  volatile char* vcp = &vc;
  volatile char* vcnil = nullptr;
  const FormatArgImpl args[] = {
      FormatArgImpl(xp),   FormatArgImpl(cp),  FormatArgImpl(inil),
      FormatArgImpl(cnil), FormatArgImpl(mcp), FormatArgImpl(fp),
      FormatArgImpl(fnil), FormatArgImpl(vcp), FormatArgImpl(vcnil),
  };
  struct Expectation {
    std::string out;
    const char *fmt;
  };
  const Expectation kExpect[] = {
      {StrPrint("%p", &x), "%p"},
      {StrPrint("%20p", &x), "%20p"},
      {StrPrint("%.1p", &x), "%.1p"},
      {StrPrint("%.20p", &x), "%.20p"},
      {StrPrint("%30.20p", &x), "%30.20p"},

      {StrPrint("%-p", &x), "%-p"},
      {StrPrint("%-20p", &x), "%-20p"},
      {StrPrint("%-.1p", &x), "%-.1p"},
      {StrPrint("%.20p", &x), "%.20p"},
      {StrPrint("%-30.20p", &x), "%-30.20p"},

      {StrPrint("%p", cp), "%2$p"},   // const char*
      {"(nil)", "%3$p"},              // null const char *
      {"(nil)", "%4$p"},              // null const int *
      {StrPrint("%p", mcp), "%5$p"},  // nonconst char*

      {StrPrint("%p", fp), "%6$p"},   // function pointer
      {StrPrint("%p", vcp), "%8$p"},  // function pointer

#ifndef __APPLE__
      // Apple's printf differs here (0x0 vs. nil)
      {StrPrint("%p", fnil), "%7$p"},   // null function pointer
      {StrPrint("%p", vcnil), "%9$p"},  // null function pointer
#endif
  };
  for (const Expectation &e : kExpect) {
    UntypedFormatSpecImpl format(e.fmt);
    EXPECT_EQ(e.out, FormatPack(format, absl::MakeSpan(args))) << e.fmt;
  }
}

struct Cardinal {
  enum Pos { k1 = 1, k2 = 2, k3 = 3 };
  enum Neg { kM1 = -1, kM2 = -2, kM3 = -3 };
};

TEST_F(FormatConvertTest, Enum) {
  const Cardinal::Pos k3 = Cardinal::k3;
  const Cardinal::Neg km3 = Cardinal::kM3;
  const FormatArgImpl args[] = {FormatArgImpl(k3), FormatArgImpl(km3)};
  UntypedFormatSpecImpl format("%1$d");
  UntypedFormatSpecImpl format2("%2$d");
  EXPECT_EQ("3", FormatPack(format, absl::MakeSpan(args)));
  EXPECT_EQ("-3", FormatPack(format2, absl::MakeSpan(args)));
}

template <typename T>
class TypedFormatConvertTest : public FormatConvertTest { };

TYPED_TEST_CASE_P(TypedFormatConvertTest);

std::vector<std::string> AllFlagCombinations() {
  const char kFlags[] = {'-', '#', '0', '+', ' '};
  std::vector<std::string> result;
  for (size_t fsi = 0; fsi < (1ull << ArraySize(kFlags)); ++fsi) {
    std::string flag_set;
    for (size_t fi = 0; fi < ArraySize(kFlags); ++fi)
      if (fsi & (1ull << fi))
        flag_set += kFlags[fi];
    result.push_back(flag_set);
  }
  return result;
}

TYPED_TEST_P(TypedFormatConvertTest, AllIntsWithFlags) {
  typedef TypeParam T;
  typedef typename std::make_unsigned<T>::type UnsignedT;
  using remove_volatile_t = typename std::remove_volatile<T>::type;
  const T kMin = std::numeric_limits<remove_volatile_t>::min();
  const T kMax = std::numeric_limits<remove_volatile_t>::max();
  const T kVals[] = {
      remove_volatile_t(1),
      remove_volatile_t(2),
      remove_volatile_t(3),
      remove_volatile_t(123),
      remove_volatile_t(-1),
      remove_volatile_t(-2),
      remove_volatile_t(-3),
      remove_volatile_t(-123),
      remove_volatile_t(0),
      kMax - remove_volatile_t(1),
      kMax,
      kMin + remove_volatile_t(1),
      kMin,
  };
  const char kConvChars[] = {'d', 'i', 'u', 'o', 'x', 'X'};
  const std::string kWid[] = {"", "4", "10"};
  const std::string kPrec[] = {"", ".", ".0", ".4", ".10"};

  const std::vector<std::string> flag_sets = AllFlagCombinations();

  for (size_t vi = 0; vi < ArraySize(kVals); ++vi) {
    const T val = kVals[vi];
    SCOPED_TRACE(Esc(val));
    const FormatArgImpl args[] = {FormatArgImpl(val)};
    for (size_t ci = 0; ci < ArraySize(kConvChars); ++ci) {
      const char conv_char = kConvChars[ci];
      for (size_t fsi = 0; fsi < flag_sets.size(); ++fsi) {
        const std::string &flag_set = flag_sets[fsi];
        for (size_t wi = 0; wi < ArraySize(kWid); ++wi) {
          const std::string &wid = kWid[wi];
          for (size_t pi = 0; pi < ArraySize(kPrec); ++pi) {
            const std::string &prec = kPrec[pi];

            const bool is_signed_conv = (conv_char == 'd' || conv_char == 'i');
            const bool is_unsigned_to_signed =
                !std::is_signed<T>::value && is_signed_conv;
            // Don't consider sign-related flags '+' and ' ' when doing
            // unsigned to signed conversions.
            if (is_unsigned_to_signed &&
                flag_set.find_first_of("+ ") != std::string::npos) {
              continue;
            }

            std::string new_fmt("%");
            new_fmt += flag_set;
            new_fmt += wid;
            new_fmt += prec;
            // old and new always agree up to here.
            std::string old_fmt = new_fmt;
            new_fmt += conv_char;
            std::string old_result;
            if (is_unsigned_to_signed) {
              // don't expect agreement on unsigned formatted as signed,
              // as printf can't do that conversion properly. For those
              // cases, we do expect agreement with printf with a "%u"
              // and the unsigned equivalent of 'val'.
              UnsignedT uval = val;
              old_fmt += LengthModFor(uval);
              old_fmt += "u";
              old_result = StrPrint(old_fmt.c_str(), uval);
            } else {
              old_fmt += LengthModFor(val);
              old_fmt += conv_char;
              old_result = StrPrint(old_fmt.c_str(), val);
            }

            SCOPED_TRACE(std::string() + " old_fmt: \"" + old_fmt +
                         "\"'"
                         " new_fmt: \"" +
                         new_fmt + "\"");
            UntypedFormatSpecImpl format(new_fmt);
            EXPECT_EQ(old_result, FormatPack(format, absl::MakeSpan(args)));
          }
        }
      }
    }
  }
}

TYPED_TEST_P(TypedFormatConvertTest, Char) {
  typedef TypeParam T;
  using remove_volatile_t = typename std::remove_volatile<T>::type;
  static const T kMin = std::numeric_limits<remove_volatile_t>::min();
  static const T kMax = std::numeric_limits<remove_volatile_t>::max();
  T kVals[] = {
    remove_volatile_t(1), remove_volatile_t(2), remove_volatile_t(10),
    remove_volatile_t(-1), remove_volatile_t(-2), remove_volatile_t(-10),
    remove_volatile_t(0),
    kMin + remove_volatile_t(1), kMin,
    kMax - remove_volatile_t(1), kMax
  };
  for (const T &c : kVals) {
    const FormatArgImpl args[] = {FormatArgImpl(c)};
    UntypedFormatSpecImpl format("%c");
    EXPECT_EQ(StrPrint("%c", c), FormatPack(format, absl::MakeSpan(args)));
  }
}

REGISTER_TYPED_TEST_CASE_P(TypedFormatConvertTest, AllIntsWithFlags, Char);

typedef ::testing::Types<
    int, unsigned, volatile int,
    short, unsigned short,
    long, unsigned long,
    long long, unsigned long long,
    signed char, unsigned char, char>
    AllIntTypes;
INSTANTIATE_TYPED_TEST_CASE_P(TypedFormatConvertTestWithAllIntTypes,
                              TypedFormatConvertTest, AllIntTypes);
TEST_F(FormatConvertTest, Uint128) {
  absl::uint128 v = static_cast<absl::uint128>(0x1234567890abcdef) * 1979;
  absl::uint128 max = absl::Uint128Max();
  const FormatArgImpl args[] = {FormatArgImpl(v), FormatArgImpl(max)};

  struct Case {
    const char* format;
    const char* expected;
  } cases[] = {
      {"%1$d", "2595989796776606496405"},
      {"%1$30d", "        2595989796776606496405"},
      {"%1$-30d", "2595989796776606496405        "},
      {"%1$u", "2595989796776606496405"},
      {"%1$x", "8cba9876066020f695"},
      {"%2$d", "340282366920938463463374607431768211455"},
      {"%2$u", "340282366920938463463374607431768211455"},
      {"%2$x", "ffffffffffffffffffffffffffffffff"},
  };

  for (auto c : cases) {
    UntypedFormatSpecImpl format(c.format);
    EXPECT_EQ(c.expected, FormatPack(format, absl::MakeSpan(args)));
  }
}

TEST_F(FormatConvertTest, Float) {
#if _MSC_VER
  // MSVC has a different rounding policy than us so we can't test our
  // implementation against the native one there.
  return;
#endif  // _MSC_VER

  const char *const kFormats[] = {
      "%",  "%.3",  "%8.5",   "%9",   "%.60", "%.30",   "%03",    "%+",
      "% ", "%-10", "%#15.3", "%#.0", "%.0",  "%1$*2$", "%1$.*2$"};

  std::vector<double> doubles = {0.0,
                                 -0.0,
                                 .99999999999999,
                                 99999999999999.,
                                 std::numeric_limits<double>::max(),
                                 -std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::min(),
                                 -std::numeric_limits<double>::min(),
                                 std::numeric_limits<double>::lowest(),
                                 -std::numeric_limits<double>::lowest(),
                                 std::numeric_limits<double>::epsilon(),
                                 std::numeric_limits<double>::epsilon() + 1,
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()};

#ifndef __APPLE__
  // Apple formats NaN differently (+nan) vs. (nan)
  doubles.push_back(std::nan(""));
#endif

  // Some regression tests.
  doubles.push_back(0.99999999999999989);

  if (std::numeric_limits<double>::has_denorm != std::denorm_absent) {
    doubles.push_back(std::numeric_limits<double>::denorm_min());
    doubles.push_back(-std::numeric_limits<double>::denorm_min());
  }

  for (double base :
       {1., 12., 123., 1234., 12345., 123456., 1234567., 12345678., 123456789.,
        1234567890., 12345678901., 123456789012., 1234567890123.}) {
    for (int exp = -123; exp <= 123; ++exp) {
      for (int sign : {1, -1}) {
        doubles.push_back(sign * std::ldexp(base, exp));
      }
    }
  }

  for (const char *fmt : kFormats) {
    for (char f : {'f', 'F',  //
                   'g', 'G',  //
                   'a', 'A',  //
                   'e', 'E'}) {
      std::string fmt_str = std::string(fmt) + f;
      for (double d : doubles) {
        int i = -10;
        FormatArgImpl args[2] = {FormatArgImpl(d), FormatArgImpl(i)};
        UntypedFormatSpecImpl format(fmt_str);
        // We use ASSERT_EQ here because failures are usually correlated and a
        // bug would print way too many failed expectations causing the test to
        // time out.
        ASSERT_EQ(StrPrint(fmt_str.c_str(), d, i),
                  FormatPack(format, absl::MakeSpan(args)))
            << fmt_str << " " << StrPrint("%.18g", d) << " "
            << StrPrint("%.999f", d);
      }
    }
  }
}

TEST_F(FormatConvertTest, LongDouble) {
  const char *const kFormats[] = {"%",    "%.3", "%8.5", "%9",
                                  "%.60", "%+",  "% ",   "%-10"};

  // This value is not representable in double, but it is in long double that
  // uses the extended format.
  // This is to verify that we are not truncating the value mistakenly through a
  // double.
  long double very_precise = 10000000000000000.25L;

  std::vector<long double> doubles = {
      0.0,
      -0.0,
      very_precise,
      1 / very_precise,
      std::numeric_limits<long double>::max(),
      -std::numeric_limits<long double>::max(),
      std::numeric_limits<long double>::min(),
      -std::numeric_limits<long double>::min(),
      std::numeric_limits<long double>::infinity(),
      -std::numeric_limits<long double>::infinity()};

  for (const char *fmt : kFormats) {
    for (char f : {'f', 'F',  //
                   'g', 'G',  //
                   'a', 'A',  //
                   'e', 'E'}) {
      std::string fmt_str = std::string(fmt) + 'L' + f;
      for (auto d : doubles) {
        FormatArgImpl arg(d);
        UntypedFormatSpecImpl format(fmt_str);
        // We use ASSERT_EQ here because failures are usually correlated and a
        // bug would print way too many failed expectations causing the test to
        // time out.
        ASSERT_EQ(StrPrint(fmt_str.c_str(), d),
                  FormatPack(format, {&arg, 1}))
            << fmt_str << " " << StrPrint("%.18Lg", d) << " "
            << StrPrint("%.999Lf", d);
      }
    }
  }
}

TEST_F(FormatConvertTest, IntAsFloat) {
  const int kMin = std::numeric_limits<int>::min();
  const int kMax = std::numeric_limits<int>::max();
  const int ia[] = {
    1, 2, 3, 123,
    -1, -2, -3, -123,
    0, kMax - 1, kMax, kMin + 1, kMin };
  for (const int fx : ia) {
    SCOPED_TRACE(fx);
    const FormatArgImpl args[] = {FormatArgImpl(fx)};
    struct Expectation {
      int line;
      std::string out;
      const char *fmt;
    };
    const double dx = static_cast<double>(fx);
    const Expectation kExpect[] = {
      { __LINE__, StrPrint("%f", dx), "%f" },
      { __LINE__, StrPrint("%12f", dx), "%12f" },
      { __LINE__, StrPrint("%.12f", dx), "%.12f" },
      { __LINE__, StrPrint("%12a", dx), "%12a" },
      { __LINE__, StrPrint("%.12a", dx), "%.12a" },
    };
    for (const Expectation &e : kExpect) {
      SCOPED_TRACE(e.line);
      SCOPED_TRACE(e.fmt);
      UntypedFormatSpecImpl format(e.fmt);
      EXPECT_EQ(e.out, FormatPack(format, absl::MakeSpan(args)));
    }
  }
}

template <typename T>
bool FormatFails(const char* test_format, T value) {
  std::string format_string = std::string("<<") + test_format + ">>";
  UntypedFormatSpecImpl format(format_string);

  int one = 1;
  const FormatArgImpl args[] = {FormatArgImpl(value), FormatArgImpl(one)};
  EXPECT_EQ(FormatPack(format, absl::MakeSpan(args)), "")
      << "format=" << test_format << " value=" << value;
  return FormatPack(format, absl::MakeSpan(args)).empty();
}

TEST_F(FormatConvertTest, ExpectedFailures) {
  // Int input
  EXPECT_TRUE(FormatFails("%p", 1));
  EXPECT_TRUE(FormatFails("%s", 1));
  EXPECT_TRUE(FormatFails("%n", 1));

  // Double input
  EXPECT_TRUE(FormatFails("%p", 1.));
  EXPECT_TRUE(FormatFails("%s", 1.));
  EXPECT_TRUE(FormatFails("%n", 1.));
  EXPECT_TRUE(FormatFails("%c", 1.));
  EXPECT_TRUE(FormatFails("%d", 1.));
  EXPECT_TRUE(FormatFails("%x", 1.));
  EXPECT_TRUE(FormatFails("%*d", 1.));

  // String input
  EXPECT_TRUE(FormatFails("%n", ""));
  EXPECT_TRUE(FormatFails("%c", ""));
  EXPECT_TRUE(FormatFails("%d", ""));
  EXPECT_TRUE(FormatFails("%x", ""));
  EXPECT_TRUE(FormatFails("%f", ""));
  EXPECT_TRUE(FormatFails("%*d", ""));
}

}  // namespace
}  // namespace str_format_internal
}  // namespace absl
