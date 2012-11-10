// Copyright 2008 and onwards Google Inc.  All rights reserved.

#include <limits>
using std::numeric_limits;


#include "base/integral_types.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/strtoint.h"
#include "split.h"
#include "strutil.h"
#include "util/hash/hash_jenkins_lookup2.h"

static const uint32 MIX32 = 0x12b9b0a1UL;           // pi; an arbitrary number
static const uint64 MIX64 = GG_ULONGLONG(0x2b992ddfa23249d6);  // more of pi

// ----------------------------------------------------------------------
// Hash32StringWithSeed()
// Hash64StringWithSeed()
// Hash32NumWithSeed()
// Hash64NumWithSeed()
//   These are Bob Jenkins' hash functions, one for 32 bit numbers
//   and one for 64 bit numbers.  Each takes a string as input and
//   a start seed.  Hashing the same string with two different seeds
//   should give two independent hash values.
//      The *Num*() functions just do a single mix, in order to
//   convert the given number into something *random*.
//
// Note that these methods may return any value for the given size, while
// the corresponding HashToXX() methods avoids certain reserved values.
// ----------------------------------------------------------------------

// These slow down a lot if inlined, so do not inline them  --Sanjay
//extern uint32 Hash32StringWithSeed(const char *s, uint32 len, uint32 c);
// Once again, not included, but copied from:
// http://szl.googlecode.com/svn-history/r40/trunk/src/utilities/hashutils.cc
// This is an Apache license...make sure this is ok
const uint32 kPrimes32[16] ={
  65537, 65539, 65543, 65551, 65557, 65563, 65579, 65581,
  65587, 65599, 65609, 65617, 65629, 65633, 65647, 65651,
};
uint32 Hash32StringWithSeed(const char *s, uint32 len, uint32 seed) {
  uint32 n = seed;
  size_t prime1 = 0, prime2 = 8;  // Indices into kPrimes32
  union {
    uint16 n;
    char bytes[sizeof(uint16)];
  } chunk;
  for (const char *i = s, *const end = s + len; i != end; ) {
    chunk.bytes[0] = *i++;
    chunk.bytes[1] = i == end ? 0 : *i++;
    n = n * kPrimes32[prime1++] ^ chunk.n * kPrimes32[prime2++];
    prime1 &= 0x0F;
    prime2 &= 0x0F;
  }
  return n;
}
extern uint64 Hash64StringWithSeed(const char *s, uint32 len, uint64 c);
extern uint32 Hash32StringWithSeedReferenceImplementation(const char *s,
                                                          uint32 len, uint32 c);
// ----------------------------------------------------------------------
// HashTo32()
// HashTo16()
// HashTo8()
//    These functions take various types of input (through operator
//    overloading) and return 32, 16, and 8 bit quantities, respectively.
//    The basic rule of our hashing is: always mix().  Thus, even for
//    char outputs we cast to a uint32 and mix with two arbitrary numbers.
//       As indicated in basictypes.h, there are a few illegal hash
//    values to watch out for.
//
// Note that these methods avoid returning certain reserved values, while
// the corresponding HashXXStringWithSeed() methdos may return any value.
// ----------------------------------------------------------------------

// This macro defines the HashTo32, To16, and To8 versions all in one go.
// It takes the argument list and a command that hashes your number.
// (For 16 and 8, we just mod retval before returning it.)  Example:
//    HASH_TO((char c), Hash32NumWithSeed(c, MIX32_1))
// evaluates to
//    uint32 retval;
//    retval = Hash32NumWithSeed(c, MIX32_1);
//    return retval == kIllegalHash32 ? retval-1 : retval;
//

inline uint32 Hash32NumWithSeed(uint32 num, uint32 c) {
  uint32 b = 0x9e3779b9UL;            // the golden ratio; an arbitrary value
  mix(num, b, c);
  return c;
}

inline uint64 Hash64NumWithSeed(uint64 num, uint64 c) {
  uint64 b = GG_ULONGLONG(0xe08c1d668b756f82);   // more of the golden ratio
  mix(num, b, c);
  return c;
}

#define HASH_TO(arglist, command)                              \
inline uint32 HashTo32 arglist {                               \
  uint32 retval = command;                                     \
  return retval == kIllegalHash32 ? retval-1 : retval;         \
}                                                              \
inline uint16 HashTo16 arglist {                               \
  uint16 retval16 = command >> 16;    /* take upper 16 bits */ \
  return retval16 == kIllegalHash16 ? retval16-1 : retval16;   \
}                                                              \
inline unsigned char HashTo8 arglist {                         \
  unsigned char retval8 = command >> 24;       /* take upper 8 bits */ \
  return retval8 == kIllegalHash8 ? retval8-1 : retval8;       \
}

// This defines:
// HashToXX(char *s, int slen);
// HashToXX(char c);
// etc

HASH_TO((const char *s, uint32 slen), Hash32StringWithSeed(s, slen, MIX32))
HASH_TO((const wchar_t *s, uint32 slen),
        Hash32StringWithSeed(reinterpret_cast<const char*>(s),
                             sizeof(wchar_t) * slen,
                             MIX32))
HASH_TO((char c),  Hash32NumWithSeed(static_cast<uint32>(c), MIX32))
HASH_TO((schar c),  Hash32NumWithSeed(static_cast<uint32>(c), MIX32))
HASH_TO((uint16 c), Hash32NumWithSeed(static_cast<uint32>(c), MIX32))
HASH_TO((int16 c),  Hash32NumWithSeed(static_cast<uint32>(c), MIX32))
HASH_TO((uint32 c), Hash32NumWithSeed(static_cast<uint32>(c), MIX32))
HASH_TO((int32 c),  Hash32NumWithSeed(static_cast<uint32>(c), MIX32))
HASH_TO((uint64 c), static_cast<uint32>(Hash64NumWithSeed(c, MIX64) >> 32))
HASH_TO((int64 c),  static_cast<uint32>(Hash64NumWithSeed(c, MIX64) >> 32))

#undef HASH_TO        // clean up the macro space

namespace {
// NOTE(user): we have to implement our own interator because
// insert_iterator<set<string> > does not instantiate without
// errors, perhaps since string != std::string.
// This is not a fully functional iterator, but is
// sufficient for SplitStringToIteratorUsing().
template <typename T>
struct simple_insert_iterator {
  T* t_;
  simple_insert_iterator(T* t) : t_(t) { }

  simple_insert_iterator<T>& operator=(const typename T::value_type& value) {
    t_->insert(value);
    return *this;
  }

  simple_insert_iterator<T>& operator*()     { return *this; }
  simple_insert_iterator<T>& operator++()    { return *this; }
  simple_insert_iterator<T>& operator++(int) { return *this; }
};

// Used to populate a hash_map out of pairs of consecutive strings in
// SplitStringToIterator{Using|AllowEmpty}().
template <typename T>
struct simple_hash_map_iterator {
  typedef hash_map<T, T> hashmap;
  hashmap* t;
  bool even;
  typename hashmap::iterator curr;

  simple_hash_map_iterator(hashmap* t_init) : t(t_init), even(true) {
    curr = t->begin();
  }

  simple_hash_map_iterator<T>& operator=(const T& value) {
    if (even) {
      curr = t->insert(make_pair(value, T())).first;
    } else {
      curr->second = value;
    }
    even = !even;
    return *this;
  }

  simple_hash_map_iterator<T>& operator*()       { return *this; }
  simple_hash_map_iterator<T>& operator++()      { return *this; }
  simple_hash_map_iterator<T>& operator++(int i) { return *this; }
};

}  // anonymous namespace

// ----------------------------------------------------------------------
// SplitStringIntoNPiecesAllowEmpty()
// SplitStringToIteratorAllowEmpty()
//    Split a string using a character delimiter. Append the components
//    to 'result'.  If there are consecutive delimiters, this function
//    will return corresponding empty strings. The string is split into
//    at most the specified number of pieces greedily. This means that the
//    last piece may possibly be split further. To split into as many pieces
//    as possible, specify 0 as the number of pieces.
//
//    If "full" is the empty string, yields an empty string as the only value.
//
//    If "pieces" is negative for some reason, it returns the whole string
// ----------------------------------------------------------------------
template <typename StringType, typename ITR>
static inline
void SplitStringToIteratorAllowEmpty(const StringType& full,
                                     const char* delim,
                                     int pieces,
                                     ITR& result) {
  string::size_type begin_index, end_index;
  begin_index = 0;

  for (int i=0; (i < pieces-1) || (pieces == 0); i++) {
    end_index = full.find_first_of(delim, begin_index);
    if (end_index == string::npos) {
      *result++ = full.substr(begin_index);
      return;
    }
    *result++ = full.substr(begin_index, (end_index - begin_index));
    begin_index = end_index + 1;
  }
  *result++ = full.substr(begin_index);
}

#ifdef _WIN32
#include <iterator>
//#define back_insert_iterator Platform::Collections::BackInsertIterator
#endif

void SplitStringIntoNPiecesAllowEmpty(const string& full,
                                      const char* delim,
                                      int pieces,
                                      vector<string>* result) {
  back_insert_iterator<vector<string> > it(*result);
  SplitStringToIteratorAllowEmpty(full, delim, pieces, it);
}

// ----------------------------------------------------------------------
// SplitStringAllowEmpty
// SplitStringToHashsetAllowEmpty
// SplitStringToSetAllowEmpty
// SplitStringToHashmapAllowEmpty
//    Split a string using a character delimiter. Append the components
//    to 'result'.  If there are consecutive delimiters, this function
//    will return corresponding empty strings.
// ----------------------------------------------------------------------
void SplitStringAllowEmpty(const string& full, const char* delim,
                           vector<string>* result) {
  back_insert_iterator<vector<string> > it(*result);
  SplitStringToIteratorAllowEmpty(full, delim, 0, it);
}

void SplitStringToHashsetAllowEmpty(const string& full, const char* delim,
                                    hash_set<string>* result) {
  simple_insert_iterator<hash_set<string> > it(result);
  SplitStringToIteratorAllowEmpty(full, delim, 0, it);
}

void SplitStringToSetAllowEmpty(const string& full, const char* delim,
                                set<string>* result) {
  simple_insert_iterator<set<string> > it(result);
  SplitStringToIteratorAllowEmpty(full, delim, 0, it);
}

void SplitStringToHashmapAllowEmpty(const string& full, const char* delim,
                                    hash_map<string, string>* result) {
  simple_hash_map_iterator<string> it(result);
  SplitStringToIteratorAllowEmpty(full, delim, 0, it);
}

// If we know how much to allocate for a vector of strings, we can
// allocate the vector<string> only once and directly to the right size.
// This saves in between 33-66 % of memory space needed for the result,
// and runs faster in the microbenchmarks.
//
// The reserve is only implemented for the single character delim.
//
// The implementation for counting is cut-and-pasted from
// SplitStringToIteratorUsing. I could have written my own counting iterator,
// and use the existing template function, but probably this is more clear
// and more sure to get optimized to reasonable code.
static int CalculateReserveForVector(const string& full, const char* delim) {
  int count = 0;
  if (delim[0] != '\0' && delim[1] == '\0') {
    // Optimize the common case where delim is a single character.
    char c = delim[0];
    const char* p = full.data();
    const char* end = p + full.size();
    while (p != end) {
      if (*p == c) {  // This could be optimized with hasless(v,1) trick.
        ++p;
      } else {
        while (++p != end && *p != c) {
          // Skip to the next occurence of the delimiter.
        }
        ++count;
      }
    }
  }
  return count;
}

// ----------------------------------------------------------------------
// SplitStringUsing()
// SplitStringToHashsetUsing()
// SplitStringToSetUsing()
// SplitStringToHashmapUsing()
//    Split a string using a character delimiter. Append the components
//    to 'result'.
//
// Note: For multi-character delimiters, this routine will split on *ANY* of
// the characters in the string, not the entire string as a single delimiter.
// ----------------------------------------------------------------------
template <typename StringType, typename ITR>
static inline
void SplitStringToIteratorUsing(const StringType& full,
                                const char* delim,
                                ITR& result) {
  // Optimize the common case where delim is a single character.
  if (delim[0] != '\0' && delim[1] == '\0') {
    char c = delim[0];
    const char* p = full.data();
    const char* end = p + full.size();
    while (p != end) {
      if (*p == c) {
        ++p;
      } else {
        const char* start = p;
        while (++p != end && *p != c) {
          // Skip to the next occurence of the delimiter.
        }
        *result++ = StringType(start, p - start);
      }
    }
    return;
  }

  string::size_type begin_index, end_index;
  begin_index = full.find_first_not_of(delim);
  while (begin_index != string::npos) {
    end_index = full.find_first_of(delim, begin_index);
    if (end_index == string::npos) {
      *result++ = full.substr(begin_index);
      return;
    }
    *result++ = full.substr(begin_index, (end_index - begin_index));
    begin_index = full.find_first_not_of(delim, end_index);
  }
}

void SplitStringUsing(const string& full,
                      const char* delim,
                      vector<string>* result) {
  result->reserve(result->size() + CalculateReserveForVector(full, delim));
  back_insert_iterator< vector<string> > it(*result);
  SplitStringToIteratorUsing(full, delim, it);
}

void SplitStringToHashsetUsing(const string& full, const char* delim,
                               hash_set<string>* result) {
  simple_insert_iterator<hash_set<string> > it(result);
  SplitStringToIteratorUsing(full, delim, it);
}

void SplitStringToSetUsing(const string& full, const char* delim,
                           set<string>* result) {
  simple_insert_iterator<set<string> > it(result);
  SplitStringToIteratorUsing(full, delim, it);
}

void SplitStringToHashmapUsing(const string& full, const char* delim,
                               hash_map<string, string>* result) {
  simple_hash_map_iterator<string> it(result);
  SplitStringToIteratorUsing(full, delim, it);
}

// ----------------------------------------------------------------------
// SplitOneIntToken()
// SplitOneInt32Token()
// SplitOneUint32Token()
// SplitOneInt64Token()
// SplitOneUint64Token()
// SplitOneDoubleToken()
// SplitOneFloatToken()
// SplitOneDecimalIntToken()
// SplitOneDecimalInt32Token()
// SplitOneDecimalUint32Token()
// SplitOneDecimalInt64Token()
// SplitOneDecimalUint64Token()
// SplitOneHexUint32Token()
// SplitOneHexUint64Token()
//   Mainly a stringified wrapper around strtol/strtoul/strtod
// ----------------------------------------------------------------------
// Curried functions for the macro below
static inline long strto32_0(const char * source, char ** end) {
  return strto32(source, end, 0); }
static inline unsigned long strtou32_0(const char * source, char ** end) {
  return strtou32(source, end, 0); }
static inline int64 strto64_0(const char * source, char ** end) {
  return strto64(source, end, 0); }
static inline uint64 strtou64_0(const char * source, char ** end) {
  return strtou64(source, end, 0); }
static inline long strto32_10(const char * source, char ** end) {
  return strto32(source, end, 10); }
static inline unsigned long strtou32_10(const char * source, char ** end) {
  return strtou32(source, end, 10); }
static inline int64 strto64_10(const char * source, char ** end) {
  return strto64(source, end, 10); }
static inline uint64 strtou64_10(const char * source, char ** end) {
  return strtou64(source, end, 10); }
static inline uint32 strtou32_16(const char * source, char ** end) {
  return strtou32(source, end, 16); }
static inline uint64 strtou64_16(const char * source, char ** end) {
  return strtou64(source, end, 16); }

#define DEFINE_SPLIT_ONE_NUMBER_TOKEN(name, type, function) \
bool SplitOne##name##Token(const char ** source, const char * delim, \
                           type * value) {                      \
  assert(source);                                               \
  assert(delim);                                                \
  assert(value);                                                \
  if (!*source)                                                 \
    return false;                                               \
  /* Parse int */                                               \
  char * end;                                                   \
  *value = function(*source, &end);                             \
  if (end == *source)                                           \
    return false; /* number not present at start of string */   \
  if (end[0] && !strchr(delim, end[0]))                         \
    return false; /* Garbage characters after int */            \
  /* Advance past token */                                      \
  if (*end != '\0')                                             \
    *source = const_cast<const char *>(end+1);                  \
   else                                                         \
    *source = NULL;                                             \
  return true;                                                  \
}

DEFINE_SPLIT_ONE_NUMBER_TOKEN(Int, int, strto32_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Int32, int32, strto32_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Uint32, uint32, strtou32_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Int64, int64, strto64_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Uint64, uint64, strtou64_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Double, double, strtod)
#ifdef _WIN32 // has no strtof()
// Note: does an implicit cast to float.
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Float, float, strtod)
#else
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Float, float, strtof)
#endif
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalInt, int, strto32_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalInt32, int32, strto32_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalUint32, uint32, strtou32_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalInt64, int64, strto64_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalUint64, uint64, strtou64_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(HexUint32, uint32, strtou32_16)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(HexUint64, uint64, strtou64_16)
