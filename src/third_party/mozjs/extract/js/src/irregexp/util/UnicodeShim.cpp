// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file is a subset of:
//  https://github.com/v8/v8/blob/master/src/strings/unicode.cc

#include "irregexp/RegExpShim.h"

#ifdef V8_INTL_SUPPORT
#  include "unicode/uchar.h"
#endif

namespace v8 {
namespace unibrow {

#ifndef V8_INTL_SUPPORT
static const int kStartBit = (1 << 30);
static const int kChunkBits = (1 << 13);
#endif  // !V8_INTL_SUPPORT

static const uchar kSentinel = static_cast<uchar>(-1);

/**
 * \file
 * Implementations of functions for working with Unicode.
 */

using int16_t = signed short;     // NOLINT
using uint16_t = unsigned short;  // NOLINT
using int32_t = int;              // NOLINT

#ifndef V8_INTL_SUPPORT
// All access to the character table should go through this function.
template <int D>
static inline uchar TableGet(const int32_t* table, int index) {
  return table[D * index];
}

static inline uchar GetEntry(int32_t entry) { return entry & (kStartBit - 1); }

static inline bool IsStart(int32_t entry) { return (entry & kStartBit) != 0; }

/**
 * Look up a character in the Unicode table using a mix of binary and
 * interpolation search.  For a uniformly distributed array
 * interpolation search beats binary search by a wide margin.  However,
 * in this case interpolation search degenerates because of some very
 * high values in the lower end of the table so this function uses a
 * combination.  The average number of steps to look up the information
 * about a character is around 10, slightly higher if there is no
 * information available about the character.
 */
static bool LookupPredicate(const int32_t* table, uint16_t size, uchar chr) {
  static const int kEntryDist = 1;
  uint16_t value = chr & (kChunkBits - 1);
  unsigned int low = 0;
  unsigned int high = size - 1;
  while (high != low) {
    unsigned int mid = low + ((high - low) >> 1);
    uchar current_value = GetEntry(TableGet<kEntryDist>(table, mid));
    // If we've found an entry less than or equal to this one, and the
    // next one is not also less than this one, we've arrived.
    if ((current_value <= value) &&
        (mid + 1 == size ||
         GetEntry(TableGet<kEntryDist>(table, mid + 1)) > value)) {
      low = mid;
      break;
    } else if (current_value < value) {
      low = mid + 1;
    } else if (current_value > value) {
      // If we've just checked the bottom-most value and it's not
      // the one we're looking for, we're done.
      if (mid == 0) break;
      high = mid - 1;
    }
  }
  int32_t field = TableGet<kEntryDist>(table, low);
  uchar entry = GetEntry(field);
  bool is_start = IsStart(field);
  return (entry == value) || (entry < value && is_start);
}
#endif  // !V8_INTL_SUPPORT

template <int kW>
struct MultiCharacterSpecialCase {
  static const uchar kEndOfEncoding = kSentinel;
  uchar chars[kW];
};

#ifndef V8_INTL_SUPPORT
// Look up the mapping for the given character in the specified table,
// which is of the specified length and uses the specified special case
// mapping for multi-char mappings.  The next parameter is the character
// following the one to map.  The result will be written in to the result
// buffer and the number of characters written will be returned.  Finally,
// if the allow_caching_ptr is non-null then false will be stored in
// it if the result contains multiple characters or depends on the
// context.
// If ranges are linear, a match between a start and end point is
// offset by the distance between the match and the start. Otherwise
// the result is the same as for the start point on the entire range.
template <bool ranges_are_linear, int kW>
static int LookupMapping(const int32_t* table, uint16_t size,
                         const MultiCharacterSpecialCase<kW>* multi_chars,
                         uchar chr, uchar next, uchar* result,
                         bool* allow_caching_ptr) {
  static const int kEntryDist = 2;
  uint16_t key = chr & (kChunkBits - 1);
  uint16_t chunk_start = chr - key;
  unsigned int low = 0;
  unsigned int high = size - 1;
  while (high != low) {
    unsigned int mid = low + ((high - low) >> 1);
    uchar current_value = GetEntry(TableGet<kEntryDist>(table, mid));
    // If we've found an entry less than or equal to this one, and the next one
    // is not also less than this one, we've arrived.
    if ((current_value <= key) &&
        (mid + 1 == size ||
         GetEntry(TableGet<kEntryDist>(table, mid + 1)) > key)) {
      low = mid;
      break;
    } else if (current_value < key) {
      low = mid + 1;
    } else if (current_value > key) {
      // If we've just checked the bottom-most value and it's not
      // the one we're looking for, we're done.
      if (mid == 0) break;
      high = mid - 1;
    }
  }
  int32_t field = TableGet<kEntryDist>(table, low);
  uchar entry = GetEntry(field);
  bool is_start = IsStart(field);
  bool found = (entry == key) || (entry < key && is_start);
  if (found) {
    int32_t value = table[2 * low + 1];
    if (value == 0) {
      // 0 means not present
      return 0;
    } else if ((value & 3) == 0) {
      // Low bits 0 means a constant offset from the given character.
      if (ranges_are_linear) {
        result[0] = chr + (value >> 2);
      } else {
        result[0] = entry + chunk_start + (value >> 2);
      }
      return 1;
    } else if ((value & 3) == 1) {
      // Low bits 1 means a special case mapping
      if (allow_caching_ptr) *allow_caching_ptr = false;
      const MultiCharacterSpecialCase<kW>& mapping = multi_chars[value >> 2];
      int length = 0;
      for (length = 0; length < kW; length++) {
        uchar mapped = mapping.chars[length];
        if (mapped == MultiCharacterSpecialCase<kW>::kEndOfEncoding) break;
        if (ranges_are_linear) {
          result[length] = mapped + (key - entry);
        } else {
          result[length] = mapped;
        }
      }
      return length;
    } else {
      // Low bits 2 means a really really special case
      if (allow_caching_ptr) *allow_caching_ptr = false;
      // The cases of this switch are defined in unicode.py in the
      // really_special_cases mapping.
      switch (value >> 2) {
        case 1:
          // Really special case 1: upper case sigma.  This letter
          // converts to two different lower case sigmas depending on
          // whether or not it occurs at the end of a word.
          if (next != 0 && Letter::Is(next)) {
            result[0] = 0x03C3;
          } else {
            result[0] = 0x03C2;
          }
          return 1;
        default:
          return 0;
      }
      return -1;
    }
  } else {
    return 0;
  }
}
#endif  // !V8_INTL_SUPPORT

// Letter:               point.category in ['Lu', 'Ll', 'Lt', 'Lm', 'Lo', 'Nl']
#ifdef V8_INTL_SUPPORT
bool Letter::Is(uchar c) { return static_cast<bool>(u_isalpha(c)); }
#else
static const uint16_t kLetterTable0Size = 431;
static const int32_t kLetterTable0[431] = {
    1073741889, 90,         1073741921, 122,
    170,        181,        186,        1073742016,  // NOLINT
    214,        1073742040, 246,        1073742072,
    705,        1073742534, 721,        1073742560,  // NOLINT
    740,        748,        750,        1073742704,
    884,        1073742710, 887,        1073742714,  // NOLINT
    893,        895,        902,        1073742728,
    906,        908,        1073742734, 929,  // NOLINT
    1073742755, 1013,       1073742839, 1153,
    1073742986, 1327,       1073743153, 1366,  // NOLINT
    1369,       1073743201, 1415,       1073743312,
    1514,       1073743344, 1522,       1073743392,  // NOLINT
    1610,       1073743470, 1647,       1073743473,
    1747,       1749,       1073743589, 1766,  // NOLINT
    1073743598, 1775,       1073743610, 1788,
    1791,       1808,       1073743634, 1839,  // NOLINT
    1073743693, 1957,       1969,       1073743818,
    2026,       1073743860, 2037,       2042,  // NOLINT
    1073743872, 2069,       2074,       2084,
    2088,       1073743936, 2136,       1073744032,  // NOLINT
    2226,       1073744132, 2361,       2365,
    2384,       1073744216, 2401,       1073744241,  // NOLINT
    2432,       1073744261, 2444,       1073744271,
    2448,       1073744275, 2472,       1073744298,  // NOLINT
    2480,       2482,       1073744310, 2489,
    2493,       2510,       1073744348, 2525,  // NOLINT
    1073744351, 2529,       1073744368, 2545,
    1073744389, 2570,       1073744399, 2576,  // NOLINT
    1073744403, 2600,       1073744426, 2608,
    1073744434, 2611,       1073744437, 2614,  // NOLINT
    1073744440, 2617,       1073744473, 2652,
    2654,       1073744498, 2676,       1073744517,  // NOLINT
    2701,       1073744527, 2705,       1073744531,
    2728,       1073744554, 2736,       1073744562,  // NOLINT
    2739,       1073744565, 2745,       2749,
    2768,       1073744608, 2785,       1073744645,  // NOLINT
    2828,       1073744655, 2832,       1073744659,
    2856,       1073744682, 2864,       1073744690,  // NOLINT
    2867,       1073744693, 2873,       2877,
    1073744732, 2909,       1073744735, 2913,  // NOLINT
    2929,       2947,       1073744773, 2954,
    1073744782, 2960,       1073744786, 2965,  // NOLINT
    1073744793, 2970,       2972,       1073744798,
    2975,       1073744803, 2980,       1073744808,  // NOLINT
    2986,       1073744814, 3001,       3024,
    1073744901, 3084,       1073744910, 3088,  // NOLINT
    1073744914, 3112,       1073744938, 3129,
    3133,       1073744984, 3161,       1073744992,  // NOLINT
    3169,       1073745029, 3212,       1073745038,
    3216,       1073745042, 3240,       1073745066,  // NOLINT
    3251,       1073745077, 3257,       3261,
    3294,       1073745120, 3297,       1073745137,  // NOLINT
    3314,       1073745157, 3340,       1073745166,
    3344,       1073745170, 3386,       3389,  // NOLINT
    3406,       1073745248, 3425,       1073745274,
    3455,       1073745285, 3478,       1073745306,  // NOLINT
    3505,       1073745331, 3515,       3517,
    1073745344, 3526,       1073745409, 3632,  // NOLINT
    1073745458, 3635,       1073745472, 3654,
    1073745537, 3714,       3716,       1073745543,  // NOLINT
    3720,       3722,       3725,       1073745556,
    3735,       1073745561, 3743,       1073745569,  // NOLINT
    3747,       3749,       3751,       1073745578,
    3755,       1073745581, 3760,       1073745586,  // NOLINT
    3763,       3773,       1073745600, 3780,
    3782,       1073745628, 3807,       3840,  // NOLINT
    1073745728, 3911,       1073745737, 3948,
    1073745800, 3980,       1073745920, 4138,  // NOLINT
    4159,       1073746000, 4181,       1073746010,
    4189,       4193,       1073746021, 4198,  // NOLINT
    1073746030, 4208,       1073746037, 4225,
    4238,       1073746080, 4293,       4295,  // NOLINT
    4301,       1073746128, 4346,       1073746172,
    4680,       1073746506, 4685,       1073746512,  // NOLINT
    4694,       4696,       1073746522, 4701,
    1073746528, 4744,       1073746570, 4749,  // NOLINT
    1073746576, 4784,       1073746610, 4789,
    1073746616, 4798,       4800,       1073746626,  // NOLINT
    4805,       1073746632, 4822,       1073746648,
    4880,       1073746706, 4885,       1073746712,  // NOLINT
    4954,       1073746816, 5007,       1073746848,
    5108,       1073746945, 5740,       1073747567,  // NOLINT
    5759,       1073747585, 5786,       1073747616,
    5866,       1073747694, 5880,       1073747712,  // NOLINT
    5900,       1073747726, 5905,       1073747744,
    5937,       1073747776, 5969,       1073747808,  // NOLINT
    5996,       1073747822, 6000,       1073747840,
    6067,       6103,       6108,       1073748000,  // NOLINT
    6263,       1073748096, 6312,       6314,
    1073748144, 6389,       1073748224, 6430,  // NOLINT
    1073748304, 6509,       1073748336, 6516,
    1073748352, 6571,       1073748417, 6599,  // NOLINT
    1073748480, 6678,       1073748512, 6740,
    6823,       1073748741, 6963,       1073748805,  // NOLINT
    6987,       1073748867, 7072,       1073748910,
    7087,       1073748922, 7141,       1073748992,  // NOLINT
    7203,       1073749069, 7247,       1073749082,
    7293,       1073749225, 7404,       1073749230,  // NOLINT
    7409,       1073749237, 7414,       1073749248,
    7615,       1073749504, 7957,       1073749784,  // NOLINT
    7965,       1073749792, 8005,       1073749832,
    8013,       1073749840, 8023,       8025,  // NOLINT
    8027,       8029,       1073749855, 8061,
    1073749888, 8116,       1073749942, 8124,  // NOLINT
    8126,       1073749954, 8132,       1073749958,
    8140,       1073749968, 8147,       1073749974,  // NOLINT
    8155,       1073749984, 8172,       1073750002,
    8180,       1073750006, 8188};  // NOLINT
static const uint16_t kLetterTable1Size = 87;
static const int32_t kLetterTable1[87] = {
    113,        127,        1073741968, 156,
    258,        263,        1073742090, 275,  // NOLINT
    277,        1073742105, 285,        292,
    294,        296,        1073742122, 301,  // NOLINT
    1073742127, 313,        1073742140, 319,
    1073742149, 329,        334,        1073742176,  // NOLINT
    392,        1073744896, 3118,       1073744944,
    3166,       1073744992, 3300,       1073745131,  // NOLINT
    3310,       1073745138, 3315,       1073745152,
    3365,       3367,       3373,       1073745200,  // NOLINT
    3431,       3439,       1073745280, 3478,
    1073745312, 3494,       1073745320, 3502,  // NOLINT
    1073745328, 3510,       1073745336, 3518,
    1073745344, 3526,       1073745352, 3534,  // NOLINT
    1073745360, 3542,       1073745368, 3550,
    3631,       1073745925, 4103,       1073745953,  // NOLINT
    4137,       1073745969, 4149,       1073745976,
    4156,       1073745985, 4246,       1073746077,  // NOLINT
    4255,       1073746081, 4346,       1073746172,
    4351,       1073746181, 4397,       1073746225,  // NOLINT
    4494,       1073746336, 4538,       1073746416,
    4607,       1073746944, 8191};  // NOLINT
static const uint16_t kLetterTable2Size = 4;
static const int32_t kLetterTable2[4] = {1073741824, 3509, 1073745408,
                                         8191};  // NOLINT
static const uint16_t kLetterTable3Size = 2;
static const int32_t kLetterTable3[2] = {1073741824, 8191};  // NOLINT
static const uint16_t kLetterTable4Size = 2;
static const int32_t kLetterTable4[2] = {1073741824, 8140};  // NOLINT
static const uint16_t kLetterTable5Size = 100;
static const int32_t kLetterTable5[100] = {
    1073741824, 1164,       1073743056, 1277,
    1073743104, 1548,       1073743376, 1567,  // NOLINT
    1073743402, 1579,       1073743424, 1646,
    1073743487, 1693,       1073743520, 1775,  // NOLINT
    1073743639, 1823,       1073743650, 1928,
    1073743755, 1934,       1073743760, 1965,  // NOLINT
    1073743792, 1969,       1073743863, 2049,
    1073743875, 2053,       1073743879, 2058,  // NOLINT
    1073743884, 2082,       1073743936, 2163,
    1073744002, 2227,       1073744114, 2295,  // NOLINT
    2299,       1073744138, 2341,       1073744176,
    2374,       1073744224, 2428,       1073744260,  // NOLINT
    2482,       2511,       1073744352, 2532,
    1073744358, 2543,       1073744378, 2558,  // NOLINT
    1073744384, 2600,       1073744448, 2626,
    1073744452, 2635,       1073744480, 2678,  // NOLINT
    2682,       1073744510, 2735,       2737,
    1073744565, 2742,       1073744569, 2749,  // NOLINT
    2752,       2754,       1073744603, 2781,
    1073744608, 2794,       1073744626, 2804,  // NOLINT
    1073744641, 2822,       1073744649, 2830,
    1073744657, 2838,       1073744672, 2854,  // NOLINT
    1073744680, 2862,       1073744688, 2906,
    1073744732, 2911,       1073744740, 2917,   // NOLINT
    1073744832, 3042,       1073744896, 8191};  // NOLINT
static const uint16_t kLetterTable6Size = 6;
static const int32_t kLetterTable6[6] = {1073741824, 6051, 1073747888, 6086,
                                         1073747915, 6139};  // NOLINT
static const uint16_t kLetterTable7Size = 48;
static const int32_t kLetterTable7[48] = {
    1073748224, 6765,       1073748592, 6873,
    1073748736, 6918,       1073748755, 6935,  // NOLINT
    6941,       1073748767, 6952,       1073748778,
    6966,       1073748792, 6972,       6974,  // NOLINT
    1073748800, 6977,       1073748803, 6980,
    1073748806, 7089,       1073748947, 7485,  // NOLINT
    1073749328, 7567,       1073749394, 7623,
    1073749488, 7675,       1073749616, 7796,  // NOLINT
    1073749622, 7932,       1073749793, 7994,
    1073749825, 8026,       1073749862, 8126,  // NOLINT
    1073749954, 8135,       1073749962, 8143,
    1073749970, 8151,       1073749978, 8156};  // NOLINT
bool Letter::Is(uchar c) {
  int chunk_index = c >> 13;
  switch (chunk_index) {
    case 0:
      return LookupPredicate(kLetterTable0, kLetterTable0Size, c);
    case 1:
      return LookupPredicate(kLetterTable1, kLetterTable1Size, c);
    case 2:
      return LookupPredicate(kLetterTable2, kLetterTable2Size, c);
    case 3:
      return LookupPredicate(kLetterTable3, kLetterTable3Size, c);
    case 4:
      return LookupPredicate(kLetterTable4, kLetterTable4Size, c);
    case 5:
      return LookupPredicate(kLetterTable5, kLetterTable5Size, c);
    case 6:
      return LookupPredicate(kLetterTable6, kLetterTable6Size, c);
    case 7:
      return LookupPredicate(kLetterTable7, kLetterTable7Size, c);
    default:
      return false;
  }
}
#endif

#ifndef V8_INTL_SUPPORT

static const MultiCharacterSpecialCase<1> kEcma262CanonicalizeMultiStrings0[1] =
    {                                                        // NOLINT
        {{kSentinel}}};                                      // NOLINT
static const uint16_t kEcma262CanonicalizeTable0Size = 498;  // NOLINT
static const int32_t kEcma262CanonicalizeTable0[996] = {
    1073741921, -128,   122,        -128,   181,        2972,
    1073742048, -128,   246,        -128,   1073742072, -128,
    254,        -128,   255,        484,  // NOLINT
    257,        -4,     259,        -4,     261,        -4,
    263,        -4,     265,        -4,     267,        -4,
    269,        -4,     271,        -4,  // NOLINT
    273,        -4,     275,        -4,     277,        -4,
    279,        -4,     281,        -4,     283,        -4,
    285,        -4,     287,        -4,  // NOLINT
    289,        -4,     291,        -4,     293,        -4,
    295,        -4,     297,        -4,     299,        -4,
    301,        -4,     303,        -4,  // NOLINT
    307,        -4,     309,        -4,     311,        -4,
    314,        -4,     316,        -4,     318,        -4,
    320,        -4,     322,        -4,  // NOLINT
    324,        -4,     326,        -4,     328,        -4,
    331,        -4,     333,        -4,     335,        -4,
    337,        -4,     339,        -4,  // NOLINT
    341,        -4,     343,        -4,     345,        -4,
    347,        -4,     349,        -4,     351,        -4,
    353,        -4,     355,        -4,  // NOLINT
    357,        -4,     359,        -4,     361,        -4,
    363,        -4,     365,        -4,     367,        -4,
    369,        -4,     371,        -4,  // NOLINT
    373,        -4,     375,        -4,     378,        -4,
    380,        -4,     382,        -4,     384,        780,
    387,        -4,     389,        -4,  // NOLINT
    392,        -4,     396,        -4,     402,        -4,
    405,        388,    409,        -4,     410,        652,
    414,        520,    417,        -4,  // NOLINT
    419,        -4,     421,        -4,     424,        -4,
    429,        -4,     432,        -4,     436,        -4,
    438,        -4,     441,        -4,  // NOLINT
    445,        -4,     447,        224,    453,        -4,
    454,        -8,     456,        -4,     457,        -8,
    459,        -4,     460,        -8,  // NOLINT
    462,        -4,     464,        -4,     466,        -4,
    468,        -4,     470,        -4,     472,        -4,
    474,        -4,     476,        -4,  // NOLINT
    477,        -316,   479,        -4,     481,        -4,
    483,        -4,     485,        -4,     487,        -4,
    489,        -4,     491,        -4,  // NOLINT
    493,        -4,     495,        -4,     498,        -4,
    499,        -8,     501,        -4,     505,        -4,
    507,        -4,     509,        -4,  // NOLINT
    511,        -4,     513,        -4,     515,        -4,
    517,        -4,     519,        -4,     521,        -4,
    523,        -4,     525,        -4,  // NOLINT
    527,        -4,     529,        -4,     531,        -4,
    533,        -4,     535,        -4,     537,        -4,
    539,        -4,     541,        -4,  // NOLINT
    543,        -4,     547,        -4,     549,        -4,
    551,        -4,     553,        -4,     555,        -4,
    557,        -4,     559,        -4,  // NOLINT
    561,        -4,     563,        -4,     572,        -4,
    1073742399, 43260,  576,        43260,  578,        -4,
    583,        -4,     585,        -4,  // NOLINT
    587,        -4,     589,        -4,     591,        -4,
    592,        43132,  593,        43120,  594,        43128,
    595,        -840,   596,        -824,  // NOLINT
    1073742422, -820,   599,        -820,   601,        -808,
    603,        -812,   604,        169276, 608,        -820,
    609,        169260, 611,        -828,  // NOLINT
    613,        169120, 614,        169232, 616,        -836,
    617,        -844,   619,        42972,  620,        169220,
    623,        -844,   625,        42996,  // NOLINT
    626,        -852,   629,        -856,   637,        42908,
    640,        -872,   643,        -872,   647,        169128,
    648,        -872,   649,        -276,  // NOLINT
    1073742474, -868,   651,        -868,   652,        -284,
    658,        -876,   670,        169032, 837,        336,
    881,        -4,     883,        -4,  // NOLINT
    887,        -4,     1073742715, 520,    893,        520,
    940,        -152,   1073742765, -148,   943,        -148,
    1073742769, -128,   961,        -128,  // NOLINT
    962,        -124,   1073742787, -128,   971,        -128,
    972,        -256,   1073742797, -252,   974,        -252,
    976,        -248,   977,        -228,  // NOLINT
    981,        -188,   982,        -216,   983,        -32,
    985,        -4,     987,        -4,     989,        -4,
    991,        -4,     993,        -4,  // NOLINT
    995,        -4,     997,        -4,     999,        -4,
    1001,       -4,     1003,       -4,     1005,       -4,
    1007,       -4,     1008,       -344,  // NOLINT
    1009,       -320,   1010,       28,     1011,       -464,
    1013,       -384,   1016,       -4,     1019,       -4,
    1073742896, -128,   1103,       -128,  // NOLINT
    1073742928, -320,   1119,       -320,   1121,       -4,
    1123,       -4,     1125,       -4,     1127,       -4,
    1129,       -4,     1131,       -4,  // NOLINT
    1133,       -4,     1135,       -4,     1137,       -4,
    1139,       -4,     1141,       -4,     1143,       -4,
    1145,       -4,     1147,       -4,  // NOLINT
    1149,       -4,     1151,       -4,     1153,       -4,
    1163,       -4,     1165,       -4,     1167,       -4,
    1169,       -4,     1171,       -4,  // NOLINT
    1173,       -4,     1175,       -4,     1177,       -4,
    1179,       -4,     1181,       -4,     1183,       -4,
    1185,       -4,     1187,       -4,  // NOLINT
    1189,       -4,     1191,       -4,     1193,       -4,
    1195,       -4,     1197,       -4,     1199,       -4,
    1201,       -4,     1203,       -4,  // NOLINT
    1205,       -4,     1207,       -4,     1209,       -4,
    1211,       -4,     1213,       -4,     1215,       -4,
    1218,       -4,     1220,       -4,  // NOLINT
    1222,       -4,     1224,       -4,     1226,       -4,
    1228,       -4,     1230,       -4,     1231,       -60,
    1233,       -4,     1235,       -4,  // NOLINT
    1237,       -4,     1239,       -4,     1241,       -4,
    1243,       -4,     1245,       -4,     1247,       -4,
    1249,       -4,     1251,       -4,  // NOLINT
    1253,       -4,     1255,       -4,     1257,       -4,
    1259,       -4,     1261,       -4,     1263,       -4,
    1265,       -4,     1267,       -4,  // NOLINT
    1269,       -4,     1271,       -4,     1273,       -4,
    1275,       -4,     1277,       -4,     1279,       -4,
    1281,       -4,     1283,       -4,  // NOLINT
    1285,       -4,     1287,       -4,     1289,       -4,
    1291,       -4,     1293,       -4,     1295,       -4,
    1297,       -4,     1299,       -4,  // NOLINT
    1301,       -4,     1303,       -4,     1305,       -4,
    1307,       -4,     1309,       -4,     1311,       -4,
    1313,       -4,     1315,       -4,  // NOLINT
    1317,       -4,     1319,       -4,     1321,       -4,
    1323,       -4,     1325,       -4,     1327,       -4,
    1073743201, -192,   1414,       -192,  // NOLINT
    7545,       141328, 7549,       15256,  7681,       -4,
    7683,       -4,     7685,       -4,     7687,       -4,
    7689,       -4,     7691,       -4,  // NOLINT
    7693,       -4,     7695,       -4,     7697,       -4,
    7699,       -4,     7701,       -4,     7703,       -4,
    7705,       -4,     7707,       -4,  // NOLINT
    7709,       -4,     7711,       -4,     7713,       -4,
    7715,       -4,     7717,       -4,     7719,       -4,
    7721,       -4,     7723,       -4,  // NOLINT
    7725,       -4,     7727,       -4,     7729,       -4,
    7731,       -4,     7733,       -4,     7735,       -4,
    7737,       -4,     7739,       -4,  // NOLINT
    7741,       -4,     7743,       -4,     7745,       -4,
    7747,       -4,     7749,       -4,     7751,       -4,
    7753,       -4,     7755,       -4,  // NOLINT
    7757,       -4,     7759,       -4,     7761,       -4,
    7763,       -4,     7765,       -4,     7767,       -4,
    7769,       -4,     7771,       -4,  // NOLINT
    7773,       -4,     7775,       -4,     7777,       -4,
    7779,       -4,     7781,       -4,     7783,       -4,
    7785,       -4,     7787,       -4,  // NOLINT
    7789,       -4,     7791,       -4,     7793,       -4,
    7795,       -4,     7797,       -4,     7799,       -4,
    7801,       -4,     7803,       -4,  // NOLINT
    7805,       -4,     7807,       -4,     7809,       -4,
    7811,       -4,     7813,       -4,     7815,       -4,
    7817,       -4,     7819,       -4,  // NOLINT
    7821,       -4,     7823,       -4,     7825,       -4,
    7827,       -4,     7829,       -4,     7835,       -236,
    7841,       -4,     7843,       -4,  // NOLINT
    7845,       -4,     7847,       -4,     7849,       -4,
    7851,       -4,     7853,       -4,     7855,       -4,
    7857,       -4,     7859,       -4,  // NOLINT
    7861,       -4,     7863,       -4,     7865,       -4,
    7867,       -4,     7869,       -4,     7871,       -4,
    7873,       -4,     7875,       -4,  // NOLINT
    7877,       -4,     7879,       -4,     7881,       -4,
    7883,       -4,     7885,       -4,     7887,       -4,
    7889,       -4,     7891,       -4,  // NOLINT
    7893,       -4,     7895,       -4,     7897,       -4,
    7899,       -4,     7901,       -4,     7903,       -4,
    7905,       -4,     7907,       -4,  // NOLINT
    7909,       -4,     7911,       -4,     7913,       -4,
    7915,       -4,     7917,       -4,     7919,       -4,
    7921,       -4,     7923,       -4,  // NOLINT
    7925,       -4,     7927,       -4,     7929,       -4,
    7931,       -4,     7933,       -4,     7935,       -4,
    1073749760, 32,     7943,       32,  // NOLINT
    1073749776, 32,     7957,       32,     1073749792, 32,
    7975,       32,     1073749808, 32,     7991,       32,
    1073749824, 32,     8005,       32,  // NOLINT
    8017,       32,     8019,       32,     8021,       32,
    8023,       32,     1073749856, 32,     8039,       32,
    1073749872, 296,    8049,       296,  // NOLINT
    1073749874, 344,    8053,       344,    1073749878, 400,
    8055,       400,    1073749880, 512,    8057,       512,
    1073749882, 448,    8059,       448,  // NOLINT
    1073749884, 504,    8061,       504,    1073749936, 32,
    8113,       32,     8126,       -28820, 1073749968, 32,
    8145,       32,     1073749984, 32,   // NOLINT
    8161,       32,     8165,       28};  // NOLINT
static const MultiCharacterSpecialCase<1> kEcma262CanonicalizeMultiStrings1[1] =
    {                                                       // NOLINT
        {{kSentinel}}};                                     // NOLINT
static const uint16_t kEcma262CanonicalizeTable1Size = 73;  // NOLINT
static const int32_t kEcma262CanonicalizeTable1[146] = {
    334,        -112,   1073742192, -64,    383,        -64,
    388,        -4,     1073743056, -104,   1257,       -104,
    1073744944, -192,   3166,       -192,  // NOLINT
    3169,       -4,     3173,       -43180, 3174,       -43168,
    3176,       -4,     3178,       -4,     3180,       -4,
    3187,       -4,     3190,       -4,  // NOLINT
    3201,       -4,     3203,       -4,     3205,       -4,
    3207,       -4,     3209,       -4,     3211,       -4,
    3213,       -4,     3215,       -4,  // NOLINT
    3217,       -4,     3219,       -4,     3221,       -4,
    3223,       -4,     3225,       -4,     3227,       -4,
    3229,       -4,     3231,       -4,  // NOLINT
    3233,       -4,     3235,       -4,     3237,       -4,
    3239,       -4,     3241,       -4,     3243,       -4,
    3245,       -4,     3247,       -4,  // NOLINT
    3249,       -4,     3251,       -4,     3253,       -4,
    3255,       -4,     3257,       -4,     3259,       -4,
    3261,       -4,     3263,       -4,  // NOLINT
    3265,       -4,     3267,       -4,     3269,       -4,
    3271,       -4,     3273,       -4,     3275,       -4,
    3277,       -4,     3279,       -4,  // NOLINT
    3281,       -4,     3283,       -4,     3285,       -4,
    3287,       -4,     3289,       -4,     3291,       -4,
    3293,       -4,     3295,       -4,  // NOLINT
    3297,       -4,     3299,       -4,     3308,       -4,
    3310,       -4,     3315,       -4,     1073745152, -29056,
    3365,       -29056, 3367,       -29056,  // NOLINT
    3373,       -29056};                     // NOLINT
static const MultiCharacterSpecialCase<1> kEcma262CanonicalizeMultiStrings5[1] =
    {                                                       // NOLINT
        {{kSentinel}}};                                     // NOLINT
static const uint16_t kEcma262CanonicalizeTable5Size = 95;  // NOLINT
static const int32_t kEcma262CanonicalizeTable5[190] =
    {
        1601, -4, 1603, -4, 1605, -4, 1607, -4,
        1609, -4, 1611, -4, 1613, -4, 1615, -4,  // NOLINT
        1617, -4, 1619, -4, 1621, -4, 1623, -4,
        1625, -4, 1627, -4, 1629, -4, 1631, -4,  // NOLINT
        1633, -4, 1635, -4, 1637, -4, 1639, -4,
        1641, -4, 1643, -4, 1645, -4, 1665, -4,  // NOLINT
        1667, -4, 1669, -4, 1671, -4, 1673, -4,
        1675, -4, 1677, -4, 1679, -4, 1681, -4,  // NOLINT
        1683, -4, 1685, -4, 1687, -4, 1689, -4,
        1691, -4, 1827, -4, 1829, -4, 1831, -4,  // NOLINT
        1833, -4, 1835, -4, 1837, -4, 1839, -4,
        1843, -4, 1845, -4, 1847, -4, 1849, -4,  // NOLINT
        1851, -4, 1853, -4, 1855, -4, 1857, -4,
        1859, -4, 1861, -4, 1863, -4, 1865, -4,  // NOLINT
        1867, -4, 1869, -4, 1871, -4, 1873, -4,
        1875, -4, 1877, -4, 1879, -4, 1881, -4,  // NOLINT
        1883, -4, 1885, -4, 1887, -4, 1889, -4,
        1891, -4, 1893, -4, 1895, -4, 1897, -4,  // NOLINT
        1899, -4, 1901, -4, 1903, -4, 1914, -4,
        1916, -4, 1919, -4, 1921, -4, 1923, -4,  // NOLINT
        1925, -4, 1927, -4, 1932, -4, 1937, -4,
        1939, -4, 1943, -4, 1945, -4, 1947, -4,  // NOLINT
        1949, -4, 1951, -4, 1953, -4, 1955, -4,
        1957, -4, 1959, -4, 1961, -4};  // NOLINT
static const MultiCharacterSpecialCase<1> kEcma262CanonicalizeMultiStrings7[1] =
    {                                                      // NOLINT
        {{kSentinel}}};                                    // NOLINT
static const uint16_t kEcma262CanonicalizeTable7Size = 2;  // NOLINT
static const int32_t kEcma262CanonicalizeTable7[4] = {1073749825, -128, 8026,
                                                      -128};  // NOLINT
int Ecma262Canonicalize::Convert(uchar c, uchar n, uchar* result,
                                 bool* allow_caching_ptr) {
  int chunk_index = c >> 13;
  switch (chunk_index) {
    case 0:
      return LookupMapping<true>(
          kEcma262CanonicalizeTable0, kEcma262CanonicalizeTable0Size,
          kEcma262CanonicalizeMultiStrings0, c, n, result, allow_caching_ptr);
    case 1:
      return LookupMapping<true>(
          kEcma262CanonicalizeTable1, kEcma262CanonicalizeTable1Size,
          kEcma262CanonicalizeMultiStrings1, c, n, result, allow_caching_ptr);
    case 5:
      return LookupMapping<true>(
          kEcma262CanonicalizeTable5, kEcma262CanonicalizeTable5Size,
          kEcma262CanonicalizeMultiStrings5, c, n, result, allow_caching_ptr);
    case 7:
      return LookupMapping<true>(
          kEcma262CanonicalizeTable7, kEcma262CanonicalizeTable7Size,
          kEcma262CanonicalizeMultiStrings7, c, n, result, allow_caching_ptr);
    default:
      return 0;
  }
}

static const MultiCharacterSpecialCase<4>
    kEcma262UnCanonicalizeMultiStrings0[507] = {  // NOLINT
        {{65, 97, kSentinel}},
        {{90, 122, kSentinel}},
        {{181, 924, 956, kSentinel}},
        {{192, 224, kSentinel}},  // NOLINT
        {{214, 246, kSentinel}},
        {{216, 248, kSentinel}},
        {{222, 254, kSentinel}},
        {{255, 376, kSentinel}},  // NOLINT
        {{256, 257, kSentinel}},
        {{258, 259, kSentinel}},
        {{260, 261, kSentinel}},
        {{262, 263, kSentinel}},  // NOLINT
        {{264, 265, kSentinel}},
        {{266, 267, kSentinel}},
        {{268, 269, kSentinel}},
        {{270, 271, kSentinel}},  // NOLINT
        {{272, 273, kSentinel}},
        {{274, 275, kSentinel}},
        {{276, 277, kSentinel}},
        {{278, 279, kSentinel}},  // NOLINT
        {{280, 281, kSentinel}},
        {{282, 283, kSentinel}},
        {{284, 285, kSentinel}},
        {{286, 287, kSentinel}},  // NOLINT
        {{288, 289, kSentinel}},
        {{290, 291, kSentinel}},
        {{292, 293, kSentinel}},
        {{294, 295, kSentinel}},  // NOLINT
        {{296, 297, kSentinel}},
        {{298, 299, kSentinel}},
        {{300, 301, kSentinel}},
        {{302, 303, kSentinel}},  // NOLINT
        {{306, 307, kSentinel}},
        {{308, 309, kSentinel}},
        {{310, 311, kSentinel}},
        {{313, 314, kSentinel}},  // NOLINT
        {{315, 316, kSentinel}},
        {{317, 318, kSentinel}},
        {{319, 320, kSentinel}},
        {{321, 322, kSentinel}},  // NOLINT
        {{323, 324, kSentinel}},
        {{325, 326, kSentinel}},
        {{327, 328, kSentinel}},
        {{330, 331, kSentinel}},  // NOLINT
        {{332, 333, kSentinel}},
        {{334, 335, kSentinel}},
        {{336, 337, kSentinel}},
        {{338, 339, kSentinel}},  // NOLINT
        {{340, 341, kSentinel}},
        {{342, 343, kSentinel}},
        {{344, 345, kSentinel}},
        {{346, 347, kSentinel}},  // NOLINT
        {{348, 349, kSentinel}},
        {{350, 351, kSentinel}},
        {{352, 353, kSentinel}},
        {{354, 355, kSentinel}},  // NOLINT
        {{356, 357, kSentinel}},
        {{358, 359, kSentinel}},
        {{360, 361, kSentinel}},
        {{362, 363, kSentinel}},  // NOLINT
        {{364, 365, kSentinel}},
        {{366, 367, kSentinel}},
        {{368, 369, kSentinel}},
        {{370, 371, kSentinel}},  // NOLINT
        {{372, 373, kSentinel}},
        {{374, 375, kSentinel}},
        {{377, 378, kSentinel}},
        {{379, 380, kSentinel}},  // NOLINT
        {{381, 382, kSentinel}},
        {{384, 579, kSentinel}},
        {{385, 595, kSentinel}},
        {{386, 387, kSentinel}},  // NOLINT
        {{388, 389, kSentinel}},
        {{390, 596, kSentinel}},
        {{391, 392, kSentinel}},
        {{393, 598, kSentinel}},  // NOLINT
        {{394, 599, kSentinel}},
        {{395, 396, kSentinel}},
        {{398, 477, kSentinel}},
        {{399, 601, kSentinel}},  // NOLINT
        {{400, 603, kSentinel}},
        {{401, 402, kSentinel}},
        {{403, 608, kSentinel}},
        {{404, 611, kSentinel}},  // NOLINT
        {{405, 502, kSentinel}},
        {{406, 617, kSentinel}},
        {{407, 616, kSentinel}},
        {{408, 409, kSentinel}},  // NOLINT
        {{410, 573, kSentinel}},
        {{412, 623, kSentinel}},
        {{413, 626, kSentinel}},
        {{414, 544, kSentinel}},  // NOLINT
        {{415, 629, kSentinel}},
        {{416, 417, kSentinel}},
        {{418, 419, kSentinel}},
        {{420, 421, kSentinel}},  // NOLINT
        {{422, 640, kSentinel}},
        {{423, 424, kSentinel}},
        {{425, 643, kSentinel}},
        {{428, 429, kSentinel}},  // NOLINT
        {{430, 648, kSentinel}},
        {{431, 432, kSentinel}},
        {{433, 650, kSentinel}},
        {{434, 651, kSentinel}},  // NOLINT
        {{435, 436, kSentinel}},
        {{437, 438, kSentinel}},
        {{439, 658, kSentinel}},
        {{440, 441, kSentinel}},  // NOLINT
        {{444, 445, kSentinel}},
        {{447, 503, kSentinel}},
        {{452, 453, 454, kSentinel}},
        {{455, 456, 457, kSentinel}},  // NOLINT
        {{458, 459, 460, kSentinel}},
        {{461, 462, kSentinel}},
        {{463, 464, kSentinel}},
        {{465, 466, kSentinel}},  // NOLINT
        {{467, 468, kSentinel}},
        {{469, 470, kSentinel}},
        {{471, 472, kSentinel}},
        {{473, 474, kSentinel}},  // NOLINT
        {{475, 476, kSentinel}},
        {{478, 479, kSentinel}},
        {{480, 481, kSentinel}},
        {{482, 483, kSentinel}},  // NOLINT
        {{484, 485, kSentinel}},
        {{486, 487, kSentinel}},
        {{488, 489, kSentinel}},
        {{490, 491, kSentinel}},  // NOLINT
        {{492, 493, kSentinel}},
        {{494, 495, kSentinel}},
        {{497, 498, 499, kSentinel}},
        {{500, 501, kSentinel}},  // NOLINT
        {{504, 505, kSentinel}},
        {{506, 507, kSentinel}},
        {{508, 509, kSentinel}},
        {{510, 511, kSentinel}},  // NOLINT
        {{512, 513, kSentinel}},
        {{514, 515, kSentinel}},
        {{516, 517, kSentinel}},
        {{518, 519, kSentinel}},  // NOLINT
        {{520, 521, kSentinel}},
        {{522, 523, kSentinel}},
        {{524, 525, kSentinel}},
        {{526, 527, kSentinel}},  // NOLINT
        {{528, 529, kSentinel}},
        {{530, 531, kSentinel}},
        {{532, 533, kSentinel}},
        {{534, 535, kSentinel}},  // NOLINT
        {{536, 537, kSentinel}},
        {{538, 539, kSentinel}},
        {{540, 541, kSentinel}},
        {{542, 543, kSentinel}},  // NOLINT
        {{546, 547, kSentinel}},
        {{548, 549, kSentinel}},
        {{550, 551, kSentinel}},
        {{552, 553, kSentinel}},  // NOLINT
        {{554, 555, kSentinel}},
        {{556, 557, kSentinel}},
        {{558, 559, kSentinel}},
        {{560, 561, kSentinel}},  // NOLINT
        {{562, 563, kSentinel}},
        {{570, 11365, kSentinel}},
        {{571, 572, kSentinel}},
        {{574, 11366, kSentinel}},  // NOLINT
        {{575, 11390, kSentinel}},
        {{576, 11391, kSentinel}},
        {{577, 578, kSentinel}},
        {{580, 649, kSentinel}},  // NOLINT
        {{581, 652, kSentinel}},
        {{582, 583, kSentinel}},
        {{584, 585, kSentinel}},
        {{586, 587, kSentinel}},  // NOLINT
        {{588, 589, kSentinel}},
        {{590, 591, kSentinel}},
        {{592, 11375, kSentinel}},
        {{593, 11373, kSentinel}},  // NOLINT
        {{594, 11376, kSentinel}},
        {{604, 42923, kSentinel}},
        {{609, 42924, kSentinel}},
        {{613, 42893, kSentinel}},  // NOLINT
        {{614, 42922, kSentinel}},
        {{619, 11362, kSentinel}},
        {{620, 42925, kSentinel}},
        {{625, 11374, kSentinel}},  // NOLINT
        {{637, 11364, kSentinel}},
        {{647, 42929, kSentinel}},
        {{670, 42928, kSentinel}},
        {{837, 921, 953, 8126}},  // NOLINT
        {{880, 881, kSentinel}},
        {{882, 883, kSentinel}},
        {{886, 887, kSentinel}},
        {{891, 1021, kSentinel}},  // NOLINT
        {{893, 1023, kSentinel}},
        {{895, 1011, kSentinel}},
        {{902, 940, kSentinel}},
        {{904, 941, kSentinel}},  // NOLINT
        {{906, 943, kSentinel}},
        {{908, 972, kSentinel}},
        {{910, 973, kSentinel}},
        {{911, 974, kSentinel}},  // NOLINT
        {{913, 945, kSentinel}},
        {{914, 946, 976, kSentinel}},
        {{915, 947, kSentinel}},
        {{916, 948, kSentinel}},  // NOLINT
        {{917, 949, 1013, kSentinel}},
        {{918, 950, kSentinel}},
        {{919, 951, kSentinel}},
        {{920, 952, 977, kSentinel}},  // NOLINT
        {{922, 954, 1008, kSentinel}},
        {{923, 955, kSentinel}},
        {{925, 957, kSentinel}},
        {{927, 959, kSentinel}},  // NOLINT
        {{928, 960, 982, kSentinel}},
        {{929, 961, 1009, kSentinel}},
        {{931, 962, 963, kSentinel}},
        {{932, 964, kSentinel}},  // NOLINT
        {{933, 965, kSentinel}},
        {{934, 966, 981, kSentinel}},
        {{935, 967, kSentinel}},
        {{939, 971, kSentinel}},  // NOLINT
        {{975, 983, kSentinel}},
        {{984, 985, kSentinel}},
        {{986, 987, kSentinel}},
        {{988, 989, kSentinel}},  // NOLINT
        {{990, 991, kSentinel}},
        {{992, 993, kSentinel}},
        {{994, 995, kSentinel}},
        {{996, 997, kSentinel}},  // NOLINT
        {{998, 999, kSentinel}},
        {{1000, 1001, kSentinel}},
        {{1002, 1003, kSentinel}},
        {{1004, 1005, kSentinel}},  // NOLINT
        {{1006, 1007, kSentinel}},
        {{1010, 1017, kSentinel}},
        {{1015, 1016, kSentinel}},
        {{1018, 1019, kSentinel}},  // NOLINT
        {{1024, 1104, kSentinel}},
        {{1039, 1119, kSentinel}},
        {{1040, 1072, kSentinel}},
        {{1071, 1103, kSentinel}},  // NOLINT
        {{1120, 1121, kSentinel}},
        {{1122, 1123, kSentinel}},
        {{1124, 1125, kSentinel}},
        {{1126, 1127, kSentinel}},  // NOLINT
        {{1128, 1129, kSentinel}},
        {{1130, 1131, kSentinel}},
        {{1132, 1133, kSentinel}},
        {{1134, 1135, kSentinel}},  // NOLINT
        {{1136, 1137, kSentinel}},
        {{1138, 1139, kSentinel}},
        {{1140, 1141, kSentinel}},
        {{1142, 1143, kSentinel}},  // NOLINT
        {{1144, 1145, kSentinel}},
        {{1146, 1147, kSentinel}},
        {{1148, 1149, kSentinel}},
        {{1150, 1151, kSentinel}},  // NOLINT
        {{1152, 1153, kSentinel}},
        {{1162, 1163, kSentinel}},
        {{1164, 1165, kSentinel}},
        {{1166, 1167, kSentinel}},  // NOLINT
        {{1168, 1169, kSentinel}},
        {{1170, 1171, kSentinel}},
        {{1172, 1173, kSentinel}},
        {{1174, 1175, kSentinel}},  // NOLINT
        {{1176, 1177, kSentinel}},
        {{1178, 1179, kSentinel}},
        {{1180, 1181, kSentinel}},
        {{1182, 1183, kSentinel}},  // NOLINT
        {{1184, 1185, kSentinel}},
        {{1186, 1187, kSentinel}},
        {{1188, 1189, kSentinel}},
        {{1190, 1191, kSentinel}},  // NOLINT
        {{1192, 1193, kSentinel}},
        {{1194, 1195, kSentinel}},
        {{1196, 1197, kSentinel}},
        {{1198, 1199, kSentinel}},  // NOLINT
        {{1200, 1201, kSentinel}},
        {{1202, 1203, kSentinel}},
        {{1204, 1205, kSentinel}},
        {{1206, 1207, kSentinel}},  // NOLINT
        {{1208, 1209, kSentinel}},
        {{1210, 1211, kSentinel}},
        {{1212, 1213, kSentinel}},
        {{1214, 1215, kSentinel}},  // NOLINT
        {{1216, 1231, kSentinel}},
        {{1217, 1218, kSentinel}},
        {{1219, 1220, kSentinel}},
        {{1221, 1222, kSentinel}},  // NOLINT
        {{1223, 1224, kSentinel}},
        {{1225, 1226, kSentinel}},
        {{1227, 1228, kSentinel}},
        {{1229, 1230, kSentinel}},  // NOLINT
        {{1232, 1233, kSentinel}},
        {{1234, 1235, kSentinel}},
        {{1236, 1237, kSentinel}},
        {{1238, 1239, kSentinel}},  // NOLINT
        {{1240, 1241, kSentinel}},
        {{1242, 1243, kSentinel}},
        {{1244, 1245, kSentinel}},
        {{1246, 1247, kSentinel}},  // NOLINT
        {{1248, 1249, kSentinel}},
        {{1250, 1251, kSentinel}},
        {{1252, 1253, kSentinel}},
        {{1254, 1255, kSentinel}},  // NOLINT
        {{1256, 1257, kSentinel}},
        {{1258, 1259, kSentinel}},
        {{1260, 1261, kSentinel}},
        {{1262, 1263, kSentinel}},  // NOLINT
        {{1264, 1265, kSentinel}},
        {{1266, 1267, kSentinel}},
        {{1268, 1269, kSentinel}},
        {{1270, 1271, kSentinel}},  // NOLINT
        {{1272, 1273, kSentinel}},
        {{1274, 1275, kSentinel}},
        {{1276, 1277, kSentinel}},
        {{1278, 1279, kSentinel}},  // NOLINT
        {{1280, 1281, kSentinel}},
        {{1282, 1283, kSentinel}},
        {{1284, 1285, kSentinel}},
        {{1286, 1287, kSentinel}},  // NOLINT
        {{1288, 1289, kSentinel}},
        {{1290, 1291, kSentinel}},
        {{1292, 1293, kSentinel}},
        {{1294, 1295, kSentinel}},  // NOLINT
        {{1296, 1297, kSentinel}},
        {{1298, 1299, kSentinel}},
        {{1300, 1301, kSentinel}},
        {{1302, 1303, kSentinel}},  // NOLINT
        {{1304, 1305, kSentinel}},
        {{1306, 1307, kSentinel}},
        {{1308, 1309, kSentinel}},
        {{1310, 1311, kSentinel}},  // NOLINT
        {{1312, 1313, kSentinel}},
        {{1314, 1315, kSentinel}},
        {{1316, 1317, kSentinel}},
        {{1318, 1319, kSentinel}},  // NOLINT
        {{1320, 1321, kSentinel}},
        {{1322, 1323, kSentinel}},
        {{1324, 1325, kSentinel}},
        {{1326, 1327, kSentinel}},  // NOLINT
        {{1329, 1377, kSentinel}},
        {{1366, 1414, kSentinel}},
        {{4256, 11520, kSentinel}},
        {{4293, 11557, kSentinel}},  // NOLINT
        {{4295, 11559, kSentinel}},
        {{4301, 11565, kSentinel}},
        {{7545, 42877, kSentinel}},
        {{7549, 11363, kSentinel}},  // NOLINT
        {{7680, 7681, kSentinel}},
        {{7682, 7683, kSentinel}},
        {{7684, 7685, kSentinel}},
        {{7686, 7687, kSentinel}},  // NOLINT
        {{7688, 7689, kSentinel}},
        {{7690, 7691, kSentinel}},
        {{7692, 7693, kSentinel}},
        {{7694, 7695, kSentinel}},  // NOLINT
        {{7696, 7697, kSentinel}},
        {{7698, 7699, kSentinel}},
        {{7700, 7701, kSentinel}},
        {{7702, 7703, kSentinel}},  // NOLINT
        {{7704, 7705, kSentinel}},
        {{7706, 7707, kSentinel}},
        {{7708, 7709, kSentinel}},
        {{7710, 7711, kSentinel}},  // NOLINT
        {{7712, 7713, kSentinel}},
        {{7714, 7715, kSentinel}},
        {{7716, 7717, kSentinel}},
        {{7718, 7719, kSentinel}},  // NOLINT
        {{7720, 7721, kSentinel}},
        {{7722, 7723, kSentinel}},
        {{7724, 7725, kSentinel}},
        {{7726, 7727, kSentinel}},  // NOLINT
        {{7728, 7729, kSentinel}},
        {{7730, 7731, kSentinel}},
        {{7732, 7733, kSentinel}},
        {{7734, 7735, kSentinel}},  // NOLINT
        {{7736, 7737, kSentinel}},
        {{7738, 7739, kSentinel}},
        {{7740, 7741, kSentinel}},
        {{7742, 7743, kSentinel}},  // NOLINT
        {{7744, 7745, kSentinel}},
        {{7746, 7747, kSentinel}},
        {{7748, 7749, kSentinel}},
        {{7750, 7751, kSentinel}},  // NOLINT
        {{7752, 7753, kSentinel}},
        {{7754, 7755, kSentinel}},
        {{7756, 7757, kSentinel}},
        {{7758, 7759, kSentinel}},  // NOLINT
        {{7760, 7761, kSentinel}},
        {{7762, 7763, kSentinel}},
        {{7764, 7765, kSentinel}},
        {{7766, 7767, kSentinel}},  // NOLINT
        {{7768, 7769, kSentinel}},
        {{7770, 7771, kSentinel}},
        {{7772, 7773, kSentinel}},
        {{7774, 7775, kSentinel}},  // NOLINT
        {{7776, 7777, 7835, kSentinel}},
        {{7778, 7779, kSentinel}},
        {{7780, 7781, kSentinel}},
        {{7782, 7783, kSentinel}},  // NOLINT
        {{7784, 7785, kSentinel}},
        {{7786, 7787, kSentinel}},
        {{7788, 7789, kSentinel}},
        {{7790, 7791, kSentinel}},  // NOLINT
        {{7792, 7793, kSentinel}},
        {{7794, 7795, kSentinel}},
        {{7796, 7797, kSentinel}},
        {{7798, 7799, kSentinel}},  // NOLINT
        {{7800, 7801, kSentinel}},
        {{7802, 7803, kSentinel}},
        {{7804, 7805, kSentinel}},
        {{7806, 7807, kSentinel}},  // NOLINT
        {{7808, 7809, kSentinel}},
        {{7810, 7811, kSentinel}},
        {{7812, 7813, kSentinel}},
        {{7814, 7815, kSentinel}},  // NOLINT
        {{7816, 7817, kSentinel}},
        {{7818, 7819, kSentinel}},
        {{7820, 7821, kSentinel}},
        {{7822, 7823, kSentinel}},  // NOLINT
        {{7824, 7825, kSentinel}},
        {{7826, 7827, kSentinel}},
        {{7828, 7829, kSentinel}},
        {{7840, 7841, kSentinel}},  // NOLINT
        {{7842, 7843, kSentinel}},
        {{7844, 7845, kSentinel}},
        {{7846, 7847, kSentinel}},
        {{7848, 7849, kSentinel}},  // NOLINT
        {{7850, 7851, kSentinel}},
        {{7852, 7853, kSentinel}},
        {{7854, 7855, kSentinel}},
        {{7856, 7857, kSentinel}},  // NOLINT
        {{7858, 7859, kSentinel}},
        {{7860, 7861, kSentinel}},
        {{7862, 7863, kSentinel}},
        {{7864, 7865, kSentinel}},  // NOLINT
        {{7866, 7867, kSentinel}},
        {{7868, 7869, kSentinel}},
        {{7870, 7871, kSentinel}},
        {{7872, 7873, kSentinel}},  // NOLINT
        {{7874, 7875, kSentinel}},
        {{7876, 7877, kSentinel}},
        {{7878, 7879, kSentinel}},
        {{7880, 7881, kSentinel}},  // NOLINT
        {{7882, 7883, kSentinel}},
        {{7884, 7885, kSentinel}},
        {{7886, 7887, kSentinel}},
        {{7888, 7889, kSentinel}},  // NOLINT
        {{7890, 7891, kSentinel}},
        {{7892, 7893, kSentinel}},
        {{7894, 7895, kSentinel}},
        {{7896, 7897, kSentinel}},  // NOLINT
        {{7898, 7899, kSentinel}},
        {{7900, 7901, kSentinel}},
        {{7902, 7903, kSentinel}},
        {{7904, 7905, kSentinel}},  // NOLINT
        {{7906, 7907, kSentinel}},
        {{7908, 7909, kSentinel}},
        {{7910, 7911, kSentinel}},
        {{7912, 7913, kSentinel}},  // NOLINT
        {{7914, 7915, kSentinel}},
        {{7916, 7917, kSentinel}},
        {{7918, 7919, kSentinel}},
        {{7920, 7921, kSentinel}},  // NOLINT
        {{7922, 7923, kSentinel}},
        {{7924, 7925, kSentinel}},
        {{7926, 7927, kSentinel}},
        {{7928, 7929, kSentinel}},  // NOLINT
        {{7930, 7931, kSentinel}},
        {{7932, 7933, kSentinel}},
        {{7934, 7935, kSentinel}},
        {{7936, 7944, kSentinel}},  // NOLINT
        {{7943, 7951, kSentinel}},
        {{7952, 7960, kSentinel}},
        {{7957, 7965, kSentinel}},
        {{7968, 7976, kSentinel}},  // NOLINT
        {{7975, 7983, kSentinel}},
        {{7984, 7992, kSentinel}},
        {{7991, 7999, kSentinel}},
        {{8000, 8008, kSentinel}},  // NOLINT
        {{8005, 8013, kSentinel}},
        {{8017, 8025, kSentinel}},
        {{8019, 8027, kSentinel}},
        {{8021, 8029, kSentinel}},  // NOLINT
        {{8023, 8031, kSentinel}},
        {{8032, 8040, kSentinel}},
        {{8039, 8047, kSentinel}},
        {{8048, 8122, kSentinel}},  // NOLINT
        {{8049, 8123, kSentinel}},
        {{8050, 8136, kSentinel}},
        {{8053, 8139, kSentinel}},
        {{8054, 8154, kSentinel}},  // NOLINT
        {{8055, 8155, kSentinel}},
        {{8056, 8184, kSentinel}},
        {{8057, 8185, kSentinel}},
        {{8058, 8170, kSentinel}},  // NOLINT
        {{8059, 8171, kSentinel}},
        {{8060, 8186, kSentinel}},
        {{8061, 8187, kSentinel}},
        {{8112, 8120, kSentinel}},  // NOLINT
        {{8113, 8121, kSentinel}},
        {{8144, 8152, kSentinel}},
        {{8145, 8153, kSentinel}},
        {{8160, 8168, kSentinel}},  // NOLINT
        {{8161, 8169, kSentinel}},
        {{8165, 8172, kSentinel}},
        {{kSentinel}}};                                         // NOLINT
static const uint16_t kEcma262UnCanonicalizeTable0Size = 1005;  // NOLINT
static const int32_t kEcma262UnCanonicalizeTable0[2010] = {
    1073741889, 1,    90,         5,    1073741921, 1,
    122,        5,    181,        9,    1073742016, 13,
    214,        17,   1073742040, 21,  // NOLINT
    222,        25,   1073742048, 13,   246,        17,
    1073742072, 21,   254,        25,   255,        29,
    256,        33,   257,        33,  // NOLINT
    258,        37,   259,        37,   260,        41,
    261,        41,   262,        45,   263,        45,
    264,        49,   265,        49,  // NOLINT
    266,        53,   267,        53,   268,        57,
    269,        57,   270,        61,   271,        61,
    272,        65,   273,        65,  // NOLINT
    274,        69,   275,        69,   276,        73,
    277,        73,   278,        77,   279,        77,
    280,        81,   281,        81,  // NOLINT
    282,        85,   283,        85,   284,        89,
    285,        89,   286,        93,   287,        93,
    288,        97,   289,        97,  // NOLINT
    290,        101,  291,        101,  292,        105,
    293,        105,  294,        109,  295,        109,
    296,        113,  297,        113,  // NOLINT
    298,        117,  299,        117,  300,        121,
    301,        121,  302,        125,  303,        125,
    306,        129,  307,        129,  // NOLINT
    308,        133,  309,        133,  310,        137,
    311,        137,  313,        141,  314,        141,
    315,        145,  316,        145,  // NOLINT
    317,        149,  318,        149,  319,        153,
    320,        153,  321,        157,  322,        157,
    323,        161,  324,        161,  // NOLINT
    325,        165,  326,        165,  327,        169,
    328,        169,  330,        173,  331,        173,
    332,        177,  333,        177,  // NOLINT
    334,        181,  335,        181,  336,        185,
    337,        185,  338,        189,  339,        189,
    340,        193,  341,        193,  // NOLINT
    342,        197,  343,        197,  344,        201,
    345,        201,  346,        205,  347,        205,
    348,        209,  349,        209,  // NOLINT
    350,        213,  351,        213,  352,        217,
    353,        217,  354,        221,  355,        221,
    356,        225,  357,        225,  // NOLINT
    358,        229,  359,        229,  360,        233,
    361,        233,  362,        237,  363,        237,
    364,        241,  365,        241,  // NOLINT
    366,        245,  367,        245,  368,        249,
    369,        249,  370,        253,  371,        253,
    372,        257,  373,        257,  // NOLINT
    374,        261,  375,        261,  376,        29,
    377,        265,  378,        265,  379,        269,
    380,        269,  381,        273,  // NOLINT
    382,        273,  384,        277,  385,        281,
    386,        285,  387,        285,  388,        289,
    389,        289,  390,        293,  // NOLINT
    391,        297,  392,        297,  1073742217, 301,
    394,        305,  395,        309,  396,        309,
    398,        313,  399,        317,  // NOLINT
    400,        321,  401,        325,  402,        325,
    403,        329,  404,        333,  405,        337,
    406,        341,  407,        345,  // NOLINT
    408,        349,  409,        349,  410,        353,
    412,        357,  413,        361,  414,        365,
    415,        369,  416,        373,  // NOLINT
    417,        373,  418,        377,  419,        377,
    420,        381,  421,        381,  422,        385,
    423,        389,  424,        389,  // NOLINT
    425,        393,  428,        397,  429,        397,
    430,        401,  431,        405,  432,        405,
    1073742257, 409,  434,        413,  // NOLINT
    435,        417,  436,        417,  437,        421,
    438,        421,  439,        425,  440,        429,
    441,        429,  444,        433,  // NOLINT
    445,        433,  447,        437,  452,        441,
    453,        441,  454,        441,  455,        445,
    456,        445,  457,        445,  // NOLINT
    458,        449,  459,        449,  460,        449,
    461,        453,  462,        453,  463,        457,
    464,        457,  465,        461,  // NOLINT
    466,        461,  467,        465,  468,        465,
    469,        469,  470,        469,  471,        473,
    472,        473,  473,        477,  // NOLINT
    474,        477,  475,        481,  476,        481,
    477,        313,  478,        485,  479,        485,
    480,        489,  481,        489,  // NOLINT
    482,        493,  483,        493,  484,        497,
    485,        497,  486,        501,  487,        501,
    488,        505,  489,        505,  // NOLINT
    490,        509,  491,        509,  492,        513,
    493,        513,  494,        517,  495,        517,
    497,        521,  498,        521,  // NOLINT
    499,        521,  500,        525,  501,        525,
    502,        337,  503,        437,  504,        529,
    505,        529,  506,        533,  // NOLINT
    507,        533,  508,        537,  509,        537,
    510,        541,  511,        541,  512,        545,
    513,        545,  514,        549,  // NOLINT
    515,        549,  516,        553,  517,        553,
    518,        557,  519,        557,  520,        561,
    521,        561,  522,        565,  // NOLINT
    523,        565,  524,        569,  525,        569,
    526,        573,  527,        573,  528,        577,
    529,        577,  530,        581,  // NOLINT
    531,        581,  532,        585,  533,        585,
    534,        589,  535,        589,  536,        593,
    537,        593,  538,        597,  // NOLINT
    539,        597,  540,        601,  541,        601,
    542,        605,  543,        605,  544,        365,
    546,        609,  547,        609,  // NOLINT
    548,        613,  549,        613,  550,        617,
    551,        617,  552,        621,  553,        621,
    554,        625,  555,        625,  // NOLINT
    556,        629,  557,        629,  558,        633,
    559,        633,  560,        637,  561,        637,
    562,        641,  563,        641,  // NOLINT
    570,        645,  571,        649,  572,        649,
    573,        353,  574,        653,  1073742399, 657,
    576,        661,  577,        665,  // NOLINT
    578,        665,  579,        277,  580,        669,
    581,        673,  582,        677,  583,        677,
    584,        681,  585,        681,  // NOLINT
    586,        685,  587,        685,  588,        689,
    589,        689,  590,        693,  591,        693,
    592,        697,  593,        701,  // NOLINT
    594,        705,  595,        281,  596,        293,
    1073742422, 301,  599,        305,  601,        317,
    603,        321,  604,        709,  // NOLINT
    608,        329,  609,        713,  611,        333,
    613,        717,  614,        721,  616,        345,
    617,        341,  619,        725,  // NOLINT
    620,        729,  623,        357,  625,        733,
    626,        361,  629,        369,  637,        737,
    640,        385,  643,        393,  // NOLINT
    647,        741,  648,        401,  649,        669,
    1073742474, 409,  651,        413,  652,        673,
    658,        425,  670,        745,  // NOLINT
    837,        749,  880,        753,  881,        753,
    882,        757,  883,        757,  886,        761,
    887,        761,  1073742715, 765,  // NOLINT
    893,        769,  895,        773,  902,        777,
    1073742728, 781,  906,        785,  908,        789,
    1073742734, 793,  911,        797,  // NOLINT
    913,        801,  914,        805,  1073742739, 809,
    916,        813,  917,        817,  1073742742, 821,
    919,        825,  920,        829,  // NOLINT
    921,        749,  922,        833,  923,        837,
    924,        9,    1073742749, 841,  927,        845,
    928,        849,  929,        853,  // NOLINT
    931,        857,  1073742756, 861,  933,        865,
    934,        869,  1073742759, 873,  939,        877,
    940,        777,  1073742765, 781,  // NOLINT
    943,        785,  945,        801,  946,        805,
    1073742771, 809,  948,        813,  949,        817,
    1073742774, 821,  951,        825,  // NOLINT
    952,        829,  953,        749,  954,        833,
    955,        837,  956,        9,    1073742781, 841,
    959,        845,  960,        849,  // NOLINT
    961,        853,  962,        857,  963,        857,
    1073742788, 861,  965,        865,  966,        869,
    1073742791, 873,  971,        877,  // NOLINT
    972,        789,  1073742797, 793,  974,        797,
    975,        881,  976,        805,  977,        829,
    981,        869,  982,        849,  // NOLINT
    983,        881,  984,        885,  985,        885,
    986,        889,  987,        889,  988,        893,
    989,        893,  990,        897,  // NOLINT
    991,        897,  992,        901,  993,        901,
    994,        905,  995,        905,  996,        909,
    997,        909,  998,        913,  // NOLINT
    999,        913,  1000,       917,  1001,       917,
    1002,       921,  1003,       921,  1004,       925,
    1005,       925,  1006,       929,  // NOLINT
    1007,       929,  1008,       833,  1009,       853,
    1010,       933,  1011,       773,  1013,       817,
    1015,       937,  1016,       937,  // NOLINT
    1017,       933,  1018,       941,  1019,       941,
    1073742845, 765,  1023,       769,  1073742848, 945,
    1039,       949,  1073742864, 953,  // NOLINT
    1071,       957,  1073742896, 953,  1103,       957,
    1073742928, 945,  1119,       949,  1120,       961,
    1121,       961,  1122,       965,  // NOLINT
    1123,       965,  1124,       969,  1125,       969,
    1126,       973,  1127,       973,  1128,       977,
    1129,       977,  1130,       981,  // NOLINT
    1131,       981,  1132,       985,  1133,       985,
    1134,       989,  1135,       989,  1136,       993,
    1137,       993,  1138,       997,  // NOLINT
    1139,       997,  1140,       1001, 1141,       1001,
    1142,       1005, 1143,       1005, 1144,       1009,
    1145,       1009, 1146,       1013,  // NOLINT
    1147,       1013, 1148,       1017, 1149,       1017,
    1150,       1021, 1151,       1021, 1152,       1025,
    1153,       1025, 1162,       1029,  // NOLINT
    1163,       1029, 1164,       1033, 1165,       1033,
    1166,       1037, 1167,       1037, 1168,       1041,
    1169,       1041, 1170,       1045,  // NOLINT
    1171,       1045, 1172,       1049, 1173,       1049,
    1174,       1053, 1175,       1053, 1176,       1057,
    1177,       1057, 1178,       1061,  // NOLINT
    1179,       1061, 1180,       1065, 1181,       1065,
    1182,       1069, 1183,       1069, 1184,       1073,
    1185,       1073, 1186,       1077,  // NOLINT
    1187,       1077, 1188,       1081, 1189,       1081,
    1190,       1085, 1191,       1085, 1192,       1089,
    1193,       1089, 1194,       1093,  // NOLINT
    1195,       1093, 1196,       1097, 1197,       1097,
    1198,       1101, 1199,       1101, 1200,       1105,
    1201,       1105, 1202,       1109,  // NOLINT
    1203,       1109, 1204,       1113, 1205,       1113,
    1206,       1117, 1207,       1117, 1208,       1121,
    1209,       1121, 1210,       1125,  // NOLINT
    1211,       1125, 1212,       1129, 1213,       1129,
    1214,       1133, 1215,       1133, 1216,       1137,
    1217,       1141, 1218,       1141,  // NOLINT
    1219,       1145, 1220,       1145, 1221,       1149,
    1222,       1149, 1223,       1153, 1224,       1153,
    1225,       1157, 1226,       1157,  // NOLINT
    1227,       1161, 1228,       1161, 1229,       1165,
    1230,       1165, 1231,       1137, 1232,       1169,
    1233,       1169, 1234,       1173,  // NOLINT
    1235,       1173, 1236,       1177, 1237,       1177,
    1238,       1181, 1239,       1181, 1240,       1185,
    1241,       1185, 1242,       1189,  // NOLINT
    1243,       1189, 1244,       1193, 1245,       1193,
    1246,       1197, 1247,       1197, 1248,       1201,
    1249,       1201, 1250,       1205,  // NOLINT
    1251,       1205, 1252,       1209, 1253,       1209,
    1254,       1213, 1255,       1213, 1256,       1217,
    1257,       1217, 1258,       1221,  // NOLINT
    1259,       1221, 1260,       1225, 1261,       1225,
    1262,       1229, 1263,       1229, 1264,       1233,
    1265,       1233, 1266,       1237,  // NOLINT
    1267,       1237, 1268,       1241, 1269,       1241,
    1270,       1245, 1271,       1245, 1272,       1249,
    1273,       1249, 1274,       1253,  // NOLINT
    1275,       1253, 1276,       1257, 1277,       1257,
    1278,       1261, 1279,       1261, 1280,       1265,
    1281,       1265, 1282,       1269,  // NOLINT
    1283,       1269, 1284,       1273, 1285,       1273,
    1286,       1277, 1287,       1277, 1288,       1281,
    1289,       1281, 1290,       1285,  // NOLINT
    1291,       1285, 1292,       1289, 1293,       1289,
    1294,       1293, 1295,       1293, 1296,       1297,
    1297,       1297, 1298,       1301,  // NOLINT
    1299,       1301, 1300,       1305, 1301,       1305,
    1302,       1309, 1303,       1309, 1304,       1313,
    1305,       1313, 1306,       1317,  // NOLINT
    1307,       1317, 1308,       1321, 1309,       1321,
    1310,       1325, 1311,       1325, 1312,       1329,
    1313,       1329, 1314,       1333,  // NOLINT
    1315,       1333, 1316,       1337, 1317,       1337,
    1318,       1341, 1319,       1341, 1320,       1345,
    1321,       1345, 1322,       1349,  // NOLINT
    1323,       1349, 1324,       1353, 1325,       1353,
    1326,       1357, 1327,       1357, 1073743153, 1361,
    1366,       1365, 1073743201, 1361,  // NOLINT
    1414,       1365, 1073746080, 1369, 4293,       1373,
    4295,       1377, 4301,       1381, 7545,       1385,
    7549,       1389, 7680,       1393,  // NOLINT
    7681,       1393, 7682,       1397, 7683,       1397,
    7684,       1401, 7685,       1401, 7686,       1405,
    7687,       1405, 7688,       1409,  // NOLINT
    7689,       1409, 7690,       1413, 7691,       1413,
    7692,       1417, 7693,       1417, 7694,       1421,
    7695,       1421, 7696,       1425,  // NOLINT
    7697,       1425, 7698,       1429, 7699,       1429,
    7700,       1433, 7701,       1433, 7702,       1437,
    7703,       1437, 7704,       1441,  // NOLINT
    7705,       1441, 7706,       1445, 7707,       1445,
    7708,       1449, 7709,       1449, 7710,       1453,
    7711,       1453, 7712,       1457,  // NOLINT
    7713,       1457, 7714,       1461, 7715,       1461,
    7716,       1465, 7717,       1465, 7718,       1469,
    7719,       1469, 7720,       1473,  // NOLINT
    7721,       1473, 7722,       1477, 7723,       1477,
    7724,       1481, 7725,       1481, 7726,       1485,
    7727,       1485, 7728,       1489,  // NOLINT
    7729,       1489, 7730,       1493, 7731,       1493,
    7732,       1497, 7733,       1497, 7734,       1501,
    7735,       1501, 7736,       1505,  // NOLINT
    7737,       1505, 7738,       1509, 7739,       1509,
    7740,       1513, 7741,       1513, 7742,       1517,
    7743,       1517, 7744,       1521,  // NOLINT
    7745,       1521, 7746,       1525, 7747,       1525,
    7748,       1529, 7749,       1529, 7750,       1533,
    7751,       1533, 7752,       1537,  // NOLINT
    7753,       1537, 7754,       1541, 7755,       1541,
    7756,       1545, 7757,       1545, 7758,       1549,
    7759,       1549, 7760,       1553,  // NOLINT
    7761,       1553, 7762,       1557, 7763,       1557,
    7764,       1561, 7765,       1561, 7766,       1565,
    7767,       1565, 7768,       1569,  // NOLINT
    7769,       1569, 7770,       1573, 7771,       1573,
    7772,       1577, 7773,       1577, 7774,       1581,
    7775,       1581, 7776,       1585,  // NOLINT
    7777,       1585, 7778,       1589, 7779,       1589,
    7780,       1593, 7781,       1593, 7782,       1597,
    7783,       1597, 7784,       1601,  // NOLINT
    7785,       1601, 7786,       1605, 7787,       1605,
    7788,       1609, 7789,       1609, 7790,       1613,
    7791,       1613, 7792,       1617,  // NOLINT
    7793,       1617, 7794,       1621, 7795,       1621,
    7796,       1625, 7797,       1625, 7798,       1629,
    7799,       1629, 7800,       1633,  // NOLINT
    7801,       1633, 7802,       1637, 7803,       1637,
    7804,       1641, 7805,       1641, 7806,       1645,
    7807,       1645, 7808,       1649,  // NOLINT
    7809,       1649, 7810,       1653, 7811,       1653,
    7812,       1657, 7813,       1657, 7814,       1661,
    7815,       1661, 7816,       1665,  // NOLINT
    7817,       1665, 7818,       1669, 7819,       1669,
    7820,       1673, 7821,       1673, 7822,       1677,
    7823,       1677, 7824,       1681,  // NOLINT
    7825,       1681, 7826,       1685, 7827,       1685,
    7828,       1689, 7829,       1689, 7835,       1585,
    7840,       1693, 7841,       1693,  // NOLINT
    7842,       1697, 7843,       1697, 7844,       1701,
    7845,       1701, 7846,       1705, 7847,       1705,
    7848,       1709, 7849,       1709,  // NOLINT
    7850,       1713, 7851,       1713, 7852,       1717,
    7853,       1717, 7854,       1721, 7855,       1721,
    7856,       1725, 7857,       1725,  // NOLINT
    7858,       1729, 7859,       1729, 7860,       1733,
    7861,       1733, 7862,       1737, 7863,       1737,
    7864,       1741, 7865,       1741,  // NOLINT
    7866,       1745, 7867,       1745, 7868,       1749,
    7869,       1749, 7870,       1753, 7871,       1753,
    7872,       1757, 7873,       1757,  // NOLINT
    7874,       1761, 7875,       1761, 7876,       1765,
    7877,       1765, 7878,       1769, 7879,       1769,
    7880,       1773, 7881,       1773,  // NOLINT
    7882,       1777, 7883,       1777, 7884,       1781,
    7885,       1781, 7886,       1785, 7887,       1785,
    7888,       1789, 7889,       1789,  // NOLINT
    7890,       1793, 7891,       1793, 7892,       1797,
    7893,       1797, 7894,       1801, 7895,       1801,
    7896,       1805, 7897,       1805,  // NOLINT
    7898,       1809, 7899,       1809, 7900,       1813,
    7901,       1813, 7902,       1817, 7903,       1817,
    7904,       1821, 7905,       1821,  // NOLINT
    7906,       1825, 7907,       1825, 7908,       1829,
    7909,       1829, 7910,       1833, 7911,       1833,
    7912,       1837, 7913,       1837,  // NOLINT
    7914,       1841, 7915,       1841, 7916,       1845,
    7917,       1845, 7918,       1849, 7919,       1849,
    7920,       1853, 7921,       1853,  // NOLINT
    7922,       1857, 7923,       1857, 7924,       1861,
    7925,       1861, 7926,       1865, 7927,       1865,
    7928,       1869, 7929,       1869,  // NOLINT
    7930,       1873, 7931,       1873, 7932,       1877,
    7933,       1877, 7934,       1881, 7935,       1881,
    1073749760, 1885, 7943,       1889,  // NOLINT
    1073749768, 1885, 7951,       1889, 1073749776, 1893,
    7957,       1897, 1073749784, 1893, 7965,       1897,
    1073749792, 1901, 7975,       1905,  // NOLINT
    1073749800, 1901, 7983,       1905, 1073749808, 1909,
    7991,       1913, 1073749816, 1909, 7999,       1913,
    1073749824, 1917, 8005,       1921,  // NOLINT
    1073749832, 1917, 8013,       1921, 8017,       1925,
    8019,       1929, 8021,       1933, 8023,       1937,
    8025,       1925, 8027,       1929,  // NOLINT
    8029,       1933, 8031,       1937, 1073749856, 1941,
    8039,       1945, 1073749864, 1941, 8047,       1945,
    1073749872, 1949, 8049,       1953,  // NOLINT
    1073749874, 1957, 8053,       1961, 1073749878, 1965,
    8055,       1969, 1073749880, 1973, 8057,       1977,
    1073749882, 1981, 8059,       1985,  // NOLINT
    1073749884, 1989, 8061,       1993, 1073749936, 1997,
    8113,       2001, 1073749944, 1997, 8121,       2001,
    1073749946, 1949, 8123,       1953,  // NOLINT
    8126,       749,  1073749960, 1957, 8139,       1961,
    1073749968, 2005, 8145,       2009, 1073749976, 2005,
    8153,       2009, 1073749978, 1965,  // NOLINT
    8155,       1969, 1073749984, 2013, 8161,       2017,
    8165,       2021, 1073749992, 2013, 8169,       2017,
    1073749994, 1981, 8171,       1985,  // NOLINT
    8172,       2021, 1073750008, 1973, 8185,       1977,
    1073750010, 1989, 8187,       1993};  // NOLINT
static const MultiCharacterSpecialCase<2>
    kEcma262UnCanonicalizeMultiStrings1[83] = {  // NOLINT
        {{8498, 8526}},   {{8544, 8560}},   {{8559, 8575}},
        {{8579, 8580}},  // NOLINT
        {{9398, 9424}},   {{9423, 9449}},   {{11264, 11312}},
        {{11310, 11358}},  // NOLINT
        {{11360, 11361}}, {{619, 11362}},   {{7549, 11363}},
        {{637, 11364}},  // NOLINT
        {{570, 11365}},   {{574, 11366}},   {{11367, 11368}},
        {{11369, 11370}},  // NOLINT
        {{11371, 11372}}, {{593, 11373}},   {{625, 11374}},
        {{592, 11375}},  // NOLINT
        {{594, 11376}},   {{11378, 11379}}, {{11381, 11382}},
        {{575, 11390}},  // NOLINT
        {{576, 11391}},   {{11392, 11393}}, {{11394, 11395}},
        {{11396, 11397}},  // NOLINT
        {{11398, 11399}}, {{11400, 11401}}, {{11402, 11403}},
        {{11404, 11405}},  // NOLINT
        {{11406, 11407}}, {{11408, 11409}}, {{11410, 11411}},
        {{11412, 11413}},  // NOLINT
        {{11414, 11415}}, {{11416, 11417}}, {{11418, 11419}},
        {{11420, 11421}},  // NOLINT
        {{11422, 11423}}, {{11424, 11425}}, {{11426, 11427}},
        {{11428, 11429}},  // NOLINT
        {{11430, 11431}}, {{11432, 11433}}, {{11434, 11435}},
        {{11436, 11437}},  // NOLINT
        {{11438, 11439}}, {{11440, 11441}}, {{11442, 11443}},
        {{11444, 11445}},  // NOLINT
        {{11446, 11447}}, {{11448, 11449}}, {{11450, 11451}},
        {{11452, 11453}},  // NOLINT
        {{11454, 11455}}, {{11456, 11457}}, {{11458, 11459}},
        {{11460, 11461}},  // NOLINT
        {{11462, 11463}}, {{11464, 11465}}, {{11466, 11467}},
        {{11468, 11469}},  // NOLINT
        {{11470, 11471}}, {{11472, 11473}}, {{11474, 11475}},
        {{11476, 11477}},  // NOLINT
        {{11478, 11479}}, {{11480, 11481}}, {{11482, 11483}},
        {{11484, 11485}},  // NOLINT
        {{11486, 11487}}, {{11488, 11489}}, {{11490, 11491}},
        {{11499, 11500}},  // NOLINT
        {{11501, 11502}}, {{11506, 11507}}, {{4256, 11520}},
        {{4293, 11557}},                                       // NOLINT
        {{4295, 11559}},  {{4301, 11565}},  {{kSentinel}}};    // NOLINT
static const uint16_t kEcma262UnCanonicalizeTable1Size = 149;  // NOLINT
static const int32_t kEcma262UnCanonicalizeTable1[298] = {
    306,        1,   334,        1,   1073742176, 5,   367,  9,
    1073742192, 5,   383,        9,   387,        13,  388,  13,  // NOLINT
    1073743030, 17,  1231,       21,  1073743056, 17,  1257, 21,
    1073744896, 25,  3118,       29,  1073744944, 25,  3166, 29,  // NOLINT
    3168,       33,  3169,       33,  3170,       37,  3171, 41,
    3172,       45,  3173,       49,  3174,       53,  3175, 57,  // NOLINT
    3176,       57,  3177,       61,  3178,       61,  3179, 65,
    3180,       65,  3181,       69,  3182,       73,  3183, 77,  // NOLINT
    3184,       81,  3186,       85,  3187,       85,  3189, 89,
    3190,       89,  1073745022, 93,  3199,       97,  3200, 101,  // NOLINT
    3201,       101, 3202,       105, 3203,       105, 3204, 109,
    3205,       109, 3206,       113, 3207,       113, 3208, 117,  // NOLINT
    3209,       117, 3210,       121, 3211,       121, 3212, 125,
    3213,       125, 3214,       129, 3215,       129, 3216, 133,  // NOLINT
    3217,       133, 3218,       137, 3219,       137, 3220, 141,
    3221,       141, 3222,       145, 3223,       145, 3224, 149,  // NOLINT
    3225,       149, 3226,       153, 3227,       153, 3228, 157,
    3229,       157, 3230,       161, 3231,       161, 3232, 165,  // NOLINT
    3233,       165, 3234,       169, 3235,       169, 3236, 173,
    3237,       173, 3238,       177, 3239,       177, 3240, 181,  // NOLINT
    3241,       181, 3242,       185, 3243,       185, 3244, 189,
    3245,       189, 3246,       193, 3247,       193, 3248, 197,  // NOLINT
    3249,       197, 3250,       201, 3251,       201, 3252, 205,
    3253,       205, 3254,       209, 3255,       209, 3256, 213,  // NOLINT
    3257,       213, 3258,       217, 3259,       217, 3260, 221,
    3261,       221, 3262,       225, 3263,       225, 3264, 229,  // NOLINT
    3265,       229, 3266,       233, 3267,       233, 3268, 237,
    3269,       237, 3270,       241, 3271,       241, 3272, 245,  // NOLINT
    3273,       245, 3274,       249, 3275,       249, 3276, 253,
    3277,       253, 3278,       257, 3279,       257, 3280, 261,  // NOLINT
    3281,       261, 3282,       265, 3283,       265, 3284, 269,
    3285,       269, 3286,       273, 3287,       273, 3288, 277,  // NOLINT
    3289,       277, 3290,       281, 3291,       281, 3292, 285,
    3293,       285, 3294,       289, 3295,       289, 3296, 293,  // NOLINT
    3297,       293, 3298,       297, 3299,       297, 3307, 301,
    3308,       301, 3309,       305, 3310,       305, 3314, 309,  // NOLINT
    3315,       309, 1073745152, 313, 3365,       317, 3367, 321,
    3373,       325};  // NOLINT
static const MultiCharacterSpecialCase<2>
    kEcma262UnCanonicalizeMultiStrings5[104] = {  // NOLINT
        {{42560, 42561}}, {{42562, 42563}},
        {{42564, 42565}}, {{42566, 42567}},  // NOLINT
        {{42568, 42569}}, {{42570, 42571}},
        {{42572, 42573}}, {{42574, 42575}},  // NOLINT
        {{42576, 42577}}, {{42578, 42579}},
        {{42580, 42581}}, {{42582, 42583}},  // NOLINT
        {{42584, 42585}}, {{42586, 42587}},
        {{42588, 42589}}, {{42590, 42591}},  // NOLINT
        {{42592, 42593}}, {{42594, 42595}},
        {{42596, 42597}}, {{42598, 42599}},  // NOLINT
        {{42600, 42601}}, {{42602, 42603}},
        {{42604, 42605}}, {{42624, 42625}},  // NOLINT
        {{42626, 42627}}, {{42628, 42629}},
        {{42630, 42631}}, {{42632, 42633}},  // NOLINT
        {{42634, 42635}}, {{42636, 42637}},
        {{42638, 42639}}, {{42640, 42641}},  // NOLINT
        {{42642, 42643}}, {{42644, 42645}},
        {{42646, 42647}}, {{42648, 42649}},  // NOLINT
        {{42650, 42651}}, {{42786, 42787}},
        {{42788, 42789}}, {{42790, 42791}},  // NOLINT
        {{42792, 42793}}, {{42794, 42795}},
        {{42796, 42797}}, {{42798, 42799}},  // NOLINT
        {{42802, 42803}}, {{42804, 42805}},
        {{42806, 42807}}, {{42808, 42809}},  // NOLINT
        {{42810, 42811}}, {{42812, 42813}},
        {{42814, 42815}}, {{42816, 42817}},  // NOLINT
        {{42818, 42819}}, {{42820, 42821}},
        {{42822, 42823}}, {{42824, 42825}},  // NOLINT
        {{42826, 42827}}, {{42828, 42829}},
        {{42830, 42831}}, {{42832, 42833}},  // NOLINT
        {{42834, 42835}}, {{42836, 42837}},
        {{42838, 42839}}, {{42840, 42841}},  // NOLINT
        {{42842, 42843}}, {{42844, 42845}},
        {{42846, 42847}}, {{42848, 42849}},  // NOLINT
        {{42850, 42851}}, {{42852, 42853}},
        {{42854, 42855}}, {{42856, 42857}},  // NOLINT
        {{42858, 42859}}, {{42860, 42861}},
        {{42862, 42863}}, {{42873, 42874}},  // NOLINT
        {{42875, 42876}}, {{7545, 42877}},
        {{42878, 42879}}, {{42880, 42881}},  // NOLINT
        {{42882, 42883}}, {{42884, 42885}},
        {{42886, 42887}}, {{42891, 42892}},  // NOLINT
        {{613, 42893}},   {{42896, 42897}},
        {{42898, 42899}}, {{42902, 42903}},  // NOLINT
        {{42904, 42905}}, {{42906, 42907}},
        {{42908, 42909}}, {{42910, 42911}},  // NOLINT
        {{42912, 42913}}, {{42914, 42915}},
        {{42916, 42917}}, {{42918, 42919}},  // NOLINT
        {{42920, 42921}}, {{614, 42922}},
        {{604, 42923}},   {{609, 42924}},  // NOLINT
        {{620, 42925}},   {{670, 42928}},
        {{647, 42929}},   {{kSentinel}}};                      // NOLINT
static const uint16_t kEcma262UnCanonicalizeTable5Size = 198;  // NOLINT
static const int32_t
    kEcma262UnCanonicalizeTable5[396] =
        {1600, 1,   1601, 1,   1602, 5,   1603, 5,
         1604, 9,   1605, 9,   1606, 13,  1607, 13,  // NOLINT
         1608, 17,  1609, 17,  1610, 21,  1611, 21,
         1612, 25,  1613, 25,  1614, 29,  1615, 29,  // NOLINT
         1616, 33,  1617, 33,  1618, 37,  1619, 37,
         1620, 41,  1621, 41,  1622, 45,  1623, 45,  // NOLINT
         1624, 49,  1625, 49,  1626, 53,  1627, 53,
         1628, 57,  1629, 57,  1630, 61,  1631, 61,  // NOLINT
         1632, 65,  1633, 65,  1634, 69,  1635, 69,
         1636, 73,  1637, 73,  1638, 77,  1639, 77,  // NOLINT
         1640, 81,  1641, 81,  1642, 85,  1643, 85,
         1644, 89,  1645, 89,  1664, 93,  1665, 93,  // NOLINT
         1666, 97,  1667, 97,  1668, 101, 1669, 101,
         1670, 105, 1671, 105, 1672, 109, 1673, 109,  // NOLINT
         1674, 113, 1675, 113, 1676, 117, 1677, 117,
         1678, 121, 1679, 121, 1680, 125, 1681, 125,  // NOLINT
         1682, 129, 1683, 129, 1684, 133, 1685, 133,
         1686, 137, 1687, 137, 1688, 141, 1689, 141,  // NOLINT
         1690, 145, 1691, 145, 1826, 149, 1827, 149,
         1828, 153, 1829, 153, 1830, 157, 1831, 157,  // NOLINT
         1832, 161, 1833, 161, 1834, 165, 1835, 165,
         1836, 169, 1837, 169, 1838, 173, 1839, 173,  // NOLINT
         1842, 177, 1843, 177, 1844, 181, 1845, 181,
         1846, 185, 1847, 185, 1848, 189, 1849, 189,  // NOLINT
         1850, 193, 1851, 193, 1852, 197, 1853, 197,
         1854, 201, 1855, 201, 1856, 205, 1857, 205,  // NOLINT
         1858, 209, 1859, 209, 1860, 213, 1861, 213,
         1862, 217, 1863, 217, 1864, 221, 1865, 221,  // NOLINT
         1866, 225, 1867, 225, 1868, 229, 1869, 229,
         1870, 233, 1871, 233, 1872, 237, 1873, 237,  // NOLINT
         1874, 241, 1875, 241, 1876, 245, 1877, 245,
         1878, 249, 1879, 249, 1880, 253, 1881, 253,  // NOLINT
         1882, 257, 1883, 257, 1884, 261, 1885, 261,
         1886, 265, 1887, 265, 1888, 269, 1889, 269,  // NOLINT
         1890, 273, 1891, 273, 1892, 277, 1893, 277,
         1894, 281, 1895, 281, 1896, 285, 1897, 285,  // NOLINT
         1898, 289, 1899, 289, 1900, 293, 1901, 293,
         1902, 297, 1903, 297, 1913, 301, 1914, 301,  // NOLINT
         1915, 305, 1916, 305, 1917, 309, 1918, 313,
         1919, 313, 1920, 317, 1921, 317, 1922, 321,  // NOLINT
         1923, 321, 1924, 325, 1925, 325, 1926, 329,
         1927, 329, 1931, 333, 1932, 333, 1933, 337,  // NOLINT
         1936, 341, 1937, 341, 1938, 345, 1939, 345,
         1942, 349, 1943, 349, 1944, 353, 1945, 353,  // NOLINT
         1946, 357, 1947, 357, 1948, 361, 1949, 361,
         1950, 365, 1951, 365, 1952, 369, 1953, 369,  // NOLINT
         1954, 373, 1955, 373, 1956, 377, 1957, 377,
         1958, 381, 1959, 381, 1960, 385, 1961, 385,  // NOLINT
         1962, 389, 1963, 393, 1964, 397, 1965, 401,
         1968, 405, 1969, 409};  // NOLINT
static const MultiCharacterSpecialCase<2>
    kEcma262UnCanonicalizeMultiStrings7[3] = {  // NOLINT
        {{65313, 65345}},
        {{65338, 65370}},
        {{kSentinel}}};                                      // NOLINT
static const uint16_t kEcma262UnCanonicalizeTable7Size = 4;  // NOLINT
static const int32_t kEcma262UnCanonicalizeTable7[8] = {
    1073749793, 1, 7994, 5, 1073749825, 1, 8026, 5};  // NOLINT
int Ecma262UnCanonicalize::Convert(uchar c, uchar n, uchar* result,
                                   bool* allow_caching_ptr) {
  int chunk_index = c >> 13;
  switch (chunk_index) {
    case 0:
      return LookupMapping<true>(
          kEcma262UnCanonicalizeTable0, kEcma262UnCanonicalizeTable0Size,
          kEcma262UnCanonicalizeMultiStrings0, c, n, result, allow_caching_ptr);
    case 1:
      return LookupMapping<true>(
          kEcma262UnCanonicalizeTable1, kEcma262UnCanonicalizeTable1Size,
          kEcma262UnCanonicalizeMultiStrings1, c, n, result, allow_caching_ptr);
    case 5:
      return LookupMapping<true>(
          kEcma262UnCanonicalizeTable5, kEcma262UnCanonicalizeTable5Size,
          kEcma262UnCanonicalizeMultiStrings5, c, n, result, allow_caching_ptr);
    case 7:
      return LookupMapping<true>(
          kEcma262UnCanonicalizeTable7, kEcma262UnCanonicalizeTable7Size,
          kEcma262UnCanonicalizeMultiStrings7, c, n, result, allow_caching_ptr);
    default:
      return 0;
  }
}

static const MultiCharacterSpecialCase<1>
    kCanonicalizationRangeMultiStrings0[1] = {                // NOLINT
        {{kSentinel}}};                                       // NOLINT
static const uint16_t kCanonicalizationRangeTable0Size = 70;  // NOLINT
static const int32_t kCanonicalizationRangeTable0[140] = {
    1073741889, 100, 90,   0, 1073741921, 100, 122,  0,
    1073742016, 88,  214,  0, 1073742040, 24,  222,  0,  // NOLINT
    1073742048, 88,  246,  0, 1073742072, 24,  254,  0,
    1073742715, 8,   893,  0, 1073742728, 8,   906,  0,  // NOLINT
    1073742749, 8,   927,  0, 1073742759, 16,  939,  0,
    1073742765, 8,   943,  0, 1073742781, 8,   959,  0,  // NOLINT
    1073742791, 16,  971,  0, 1073742845, 8,   1023, 0,
    1073742848, 60,  1039, 0, 1073742864, 124, 1071, 0,  // NOLINT
    1073742896, 124, 1103, 0, 1073742928, 60,  1119, 0,
    1073743153, 148, 1366, 0, 1073743201, 148, 1414, 0,  // NOLINT
    1073746080, 148, 4293, 0, 1073749760, 28,  7943, 0,
    1073749768, 28,  7951, 0, 1073749776, 20,  7957, 0,  // NOLINT
    1073749784, 20,  7965, 0, 1073749792, 28,  7975, 0,
    1073749800, 28,  7983, 0, 1073749808, 28,  7991, 0,  // NOLINT
    1073749816, 28,  7999, 0, 1073749824, 20,  8005, 0,
    1073749832, 20,  8013, 0, 1073749856, 28,  8039, 0,  // NOLINT
    1073749864, 28,  8047, 0, 1073749874, 12,  8053, 0,
    1073749960, 12,  8139, 0};  // NOLINT
static const MultiCharacterSpecialCase<1>
    kCanonicalizationRangeMultiStrings1[1] = {                // NOLINT
        {{kSentinel}}};                                       // NOLINT
static const uint16_t kCanonicalizationRangeTable1Size = 14;  // NOLINT
static const int32_t kCanonicalizationRangeTable1[28] = {
    1073742176, 60,  367,  0, 1073742192, 60,  383,  0,
    1073743030, 100, 1231, 0, 1073743056, 100, 1257, 0,  // NOLINT
    1073744896, 184, 3118, 0, 1073744944, 184, 3166, 0,
    1073745152, 148, 3365, 0};  // NOLINT
static const MultiCharacterSpecialCase<1>
    kCanonicalizationRangeMultiStrings7[1] = {               // NOLINT
        {{kSentinel}}};                                      // NOLINT
static const uint16_t kCanonicalizationRangeTable7Size = 4;  // NOLINT
static const int32_t kCanonicalizationRangeTable7[8] = {
    1073749793, 100, 7994, 0, 1073749825, 100, 8026, 0};  // NOLINT
int CanonicalizationRange::Convert(uchar c, uchar n, uchar* result,
                                   bool* allow_caching_ptr) {
  int chunk_index = c >> 13;
  switch (chunk_index) {
    case 0:
      return LookupMapping<false>(
          kCanonicalizationRangeTable0, kCanonicalizationRangeTable0Size,
          kCanonicalizationRangeMultiStrings0, c, n, result, allow_caching_ptr);
    case 1:
      return LookupMapping<false>(
          kCanonicalizationRangeTable1, kCanonicalizationRangeTable1Size,
          kCanonicalizationRangeMultiStrings1, c, n, result, allow_caching_ptr);
    case 7:
      return LookupMapping<false>(
          kCanonicalizationRangeTable7, kCanonicalizationRangeTable7Size,
          kCanonicalizationRangeMultiStrings7, c, n, result, allow_caching_ptr);
    default:
      return 0;
  }
}

#endif  // !V8_INTL_SUPPORT

}  // namespace unibrow
}  // namespace v8
