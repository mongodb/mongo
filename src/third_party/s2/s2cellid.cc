// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2cellid.h"

#ifndef _WIN32
#include <pthread.h>
#endif

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <iomanip>
using std::setprecision;

#include <vector>
using std::vector;


#include "base/integral_types.h"
#include "base/logging.h"
#include "strings/strutil.h"
#include "s2.h"
#include "s2latlng.h"
#include "util/math/mathutil.h"
#include "util/math/vector2-inl.h"

#include "mongo/base/init.h"

// The following lookup tables are used to convert efficiently between an
// (i,j) cell index and the corresponding position along the Hilbert curve.
// "lookup_pos" maps 4 bits of "i", 4 bits of "j", and 2 bits representing the
// orientation of the current cell into 8 bits representing the order in which
// that subcell is visited by the Hilbert curve, plus 2 bits indicating the
// new orientation of the Hilbert curve within that subcell.  (Cell
// orientations are represented as combination of kSwapMask and kInvertMask.)
//
// "lookup_ij" is an inverted table used for mapping in the opposite
// direction.
//
// We also experimented with looking up 16 bits at a time (14 bits of position
// plus 2 of orientation) but found that smaller lookup tables gave better
// performance.  (2KB fits easily in the primary cache.)


// Values for these constants are *declared* in the *.h file. Even though
// the declaration specifies a value for the constant, that declaration
// is not a *definition* of storage for the value. Because the values are
// supplied in the declaration, we don't need the values here. Failing to
// define storage causes link errors for any code that tries to take the
// address of one of these values.
int const S2CellId::kFaceBits = 3;
int const S2CellId::kNumFaces = 6;
int const S2CellId::kMaxLevel = S2::kMaxCellLevel;
int const S2CellId::kPosBits = 2 * kMaxLevel + 1;
int const S2CellId::kMaxSize = 1 << kMaxLevel;
uint64 const S2CellId::kWrapOffset = uint64(kNumFaces) << kPosBits;

static int const kLookupBits = 4;
static int const kSwapMask = 0x01;
static int const kInvertMask = 0x02;

static uint16 lookup_pos[1 << (2 * kLookupBits + 2)];
static uint16 lookup_ij[1 << (2 * kLookupBits + 2)];

static void InitLookupCell(int level, int i, int j, int orig_orientation,
                           int pos, int orientation) {
  if (level == kLookupBits) {
    int ij = (i << kLookupBits) + j;
    lookup_pos[(ij << 2) + orig_orientation] = (pos << 2) + orientation;
    lookup_ij[(pos << 2) + orig_orientation] = (ij << 2) + orientation;
  } else {
    level++;
    i <<= 1;
    j <<= 1;
    pos <<= 2;
    int const* r = S2::kPosToIJ[orientation];
    InitLookupCell(level, i + (r[0] >> 1), j + (r[0] & 1), orig_orientation,
                   pos, orientation ^ S2::kPosToOrientation[0]);
    InitLookupCell(level, i + (r[1] >> 1), j + (r[1] & 1), orig_orientation,
                   pos + 1, orientation ^ S2::kPosToOrientation[1]);
    InitLookupCell(level, i + (r[2] >> 1), j + (r[2] & 1), orig_orientation,
                   pos + 2, orientation ^ S2::kPosToOrientation[2]);
    InitLookupCell(level, i + (r[3] >> 1), j + (r[3] & 1), orig_orientation,
                   pos + 3, orientation ^ S2::kPosToOrientation[3]);
  }
}

static void Init() {
  InitLookupCell(0, 0, 0, 0, 0, 0);
  InitLookupCell(0, 0, 0, kSwapMask, 0, kSwapMask);
  InitLookupCell(0, 0, 0, kInvertMask, 0, kInvertMask);
  InitLookupCell(0, 0, 0, kSwapMask|kInvertMask, 0, kSwapMask|kInvertMask);
}

MONGO_INITIALIZER(S2CellIdInit)(mongo::InitializerContext *context) {
    Init();
    return mongo::Status::OK();
}

int S2CellId::level() const {
  // Fast path for leaf cells.
  if (is_leaf()) return kMaxLevel;

  uint32 x = static_cast<uint32>(id_);
  int level = -1;
  if (x != 0) {
    level += 16;
  } else {
    x = static_cast<uint32>(id_ >> 32);
  }
  // We only need to look at even-numbered bits to determine the
  // level of a valid cell id.
#pragma warning(push)
#pragma warning( disable: 4146 )
  x &= -x;  // Get lowest bit.
#pragma warning(pop)
  if (x & 0x00005555) level += 8;
  if (x & 0x00550055) level += 4;
  if (x & 0x05050505) level += 2;
  if (x & 0x11111111) level += 1;
  DCHECK_GE(level, 0);
  DCHECK_LE(level, kMaxLevel);
  return level;
}

S2CellId S2CellId::advance(int64 steps) const {
  if (steps == 0) return *this;

  // We clamp the number of steps if necessary to ensure that we do not
  // advance past the End() or before the Begin() of this level.  Note that
  // min_steps and max_steps always fit in a signed 64-bit integer.

  int step_shift = 2 * (kMaxLevel - level()) + 1;
  if (steps < 0) {
    int64 min_steps = -static_cast<int64>(id_ >> step_shift);
    if (steps < min_steps) steps = min_steps;
  } else {
    int64 max_steps = (kWrapOffset + lsb() - id_) >> step_shift;
    if (steps > max_steps) steps = max_steps;
  }
  return S2CellId(id_ + (steps << step_shift));
}

S2CellId S2CellId::advance_wrap(int64 steps) const {
  DCHECK(is_valid());
  if (steps == 0) return *this;

  int step_shift = 2 * (kMaxLevel - level()) + 1;
  if (steps < 0) {
    int64 min_steps = -static_cast<int64>(id_ >> step_shift);
    if (steps < min_steps) {
      int64 step_wrap = kWrapOffset >> step_shift;
      steps %= step_wrap;
      if (steps < min_steps) steps += step_wrap;
    }
  } else {
    // Unlike advance(), we don't want to return End(level).
    int64 max_steps = (kWrapOffset - id_) >> step_shift;
    if (steps > max_steps) {
      int64 step_wrap = kWrapOffset >> step_shift;
      steps %= step_wrap;
      if (steps > max_steps) steps -= step_wrap;
    }
  }
  return S2CellId(id_ + (steps << step_shift));
}

S2CellId S2CellId::FromFacePosLevel(int face, uint64 pos, int level) {
  S2CellId cell((static_cast<uint64>(face) << kPosBits) + (pos | 1));
  return cell.parent(level);
}

string S2CellId::ToToken() const {
  // Simple implementation: convert the id to hex and strip trailing zeros.
  // Using hex has the advantage that the tokens are case-insensitive, all
  // characters are alphanumeric, no characters require any special escaping
  // in Mustang queries, and it's easy to compare cell tokens against the
  // feature ids of the corresponding features.
  //
  // Using base 64 would produce slightly shorter tokens, but for typical cell
  // sizes used during indexing (up to level 15 or so) the average savings
  // would be less than 2 bytes per cell which doesn't seem worth it.

  char digits[17];
  FastHex64ToBuffer(id_, digits);
  for (int len = 16; len > 0; --len) {
    if (digits[len-1] != '0') {
      return string(digits, len);
    }
  }
  return "X";  // Invalid hex string.
}

S2CellId S2CellId::FromToken(string const& token) {
  if (token.size() > 16) return S2CellId::None();
  char digits[17] = "0000000000000000";
  memcpy(digits, token.data(), token.size());
  return S2CellId(ParseLeadingHex64Value(digits, 0));
}

inline int S2CellId::STtoIJ(double s) {
  // Converting from floating-point to integers via static_cast is very slow
  // on Intel processors because it requires changing the rounding mode.
  // Rounding to the nearest integer using FastIntRound() is much faster.

  return max(0, min(kMaxSize - 1, MathUtil::FastIntRound(kMaxSize * s - 0.5)));
}


S2CellId S2CellId::FromFaceIJ(int face, int i, int j) {
  // Optimization notes:
  //  - Non-overlapping bit fields can be combined with either "+" or "|".
  //    Generally "+" seems to produce better code, but not always.

  // gcc doesn't have very good code generation for 64-bit operations.
  // We optimize this by computing the result as two 32-bit integers
  // and combining them at the end.  Declaring the result as an array
  // rather than local variables helps the compiler to do a better job
  // of register allocation as well.  Note that the two 32-bits halves
  // get shifted one bit to the left when they are combined.
  uint32 n[2];
  n[0] = 0;
  n[1]  = face << (kPosBits - 33);

  // Alternating faces have opposite Hilbert curve orientations; this
  // is necessary in order for all faces to have a right-handed
  // coordinate system.
  int bits = (face & kSwapMask);

  // Each iteration maps 4 bits of "i" and "j" into 8 bits of the Hilbert
  // curve position.  The lookup table transforms a 10-bit key of the form
  // "iiiijjjjoo" to a 10-bit value of the form "ppppppppoo", where the
  // letters [ijpo] denote bits of "i", "j", Hilbert curve position, and
  // Hilbert curve orientation respectively.
#define GET_BITS(k) do { \
    int const mask = (1 << kLookupBits) - 1; \
    bits += ((i >> (k * kLookupBits)) & mask) << (kLookupBits + 2); \
    bits += ((j >> (k * kLookupBits)) & mask) << 2; \
    bits = lookup_pos[bits]; \
    n[k >> 2] |= (bits >> 2) << ((k & 3) * 2 * kLookupBits); \
    bits &= (kSwapMask | kInvertMask); \
  } while (0)

  GET_BITS(7);
  GET_BITS(6);
  GET_BITS(5);
  GET_BITS(4);
  GET_BITS(3);
  GET_BITS(2);
  GET_BITS(1);
  GET_BITS(0);
#undef GET_BITS

  return S2CellId(((static_cast<uint64>(n[1]) << 32) + n[0]) * 2 + 1);
}

S2CellId S2CellId::FromPoint(S2Point const& p) {
  double u, v;
  int face = S2::XYZtoFaceUV(p, &u, &v);
  int i = STtoIJ(S2::UVtoST(u));
  int j = STtoIJ(S2::UVtoST(v));
  return FromFaceIJ(face, i, j);
}

S2CellId S2CellId::FromLatLng(S2LatLng const& ll) {
  return FromPoint(ll.ToPoint());
}

int S2CellId::ToFaceIJOrientation(int* pi, int* pj, int* orientation) const {
  int i = 0, j = 0;
  int face = this->face();
  int bits = (face & kSwapMask);

  // Each iteration maps 8 bits of the Hilbert curve position into
  // 4 bits of "i" and "j".  The lookup table transforms a key of the
  // form "ppppppppoo" to a value of the form "iiiijjjjoo", where the
  // letters [ijpo] represents bits of "i", "j", the Hilbert curve
  // position, and the Hilbert curve orientation respectively.
  //
  // On the first iteration we need to be careful to clear out the bits
  // representing the cube face.
#define GET_BITS(k) do { \
    int const nbits = (k == 7) ? (kMaxLevel - 7 * kLookupBits) : kLookupBits; \
    bits += (static_cast<int>(id_ >> (k * 2 * kLookupBits + 1)) \
             & ((1 << (2 * nbits)) - 1)) << 2; \
    bits = lookup_ij[bits]; \
    i += (bits >> (kLookupBits + 2)) << (k * kLookupBits); \
    j += ((bits >> 2) & ((1 << kLookupBits) - 1)) << (k * kLookupBits); \
    bits &= (kSwapMask | kInvertMask); \
  } while (0)

  GET_BITS(7);
  GET_BITS(6);
  GET_BITS(5);
  GET_BITS(4);
  GET_BITS(3);
  GET_BITS(2);
  GET_BITS(1);
  GET_BITS(0);
#undef GET_BITS

  *pi = i;
  *pj = j;

  if (orientation != NULL) {
    // The position of a non-leaf cell at level "n" consists of a prefix of
    // 2*n bits that identifies the cell, followed by a suffix of
    // 2*(kMaxLevel-n)+1 bits of the form 10*.  If n==kMaxLevel, the suffix is
    // just "1" and has no effect.  Otherwise, it consists of "10", followed
    // by (kMaxLevel-n-1) repetitions of "00", followed by "0".  The "10" has
    // no effect, while each occurrence of "00" has the effect of reversing
    // the kSwapMask bit.
    DCHECK_EQ(0, S2::kPosToOrientation[2]);
    DCHECK_EQ(S2::kSwapMask, S2::kPosToOrientation[0]);
    if (lsb() & GG_ULONGLONG(0x1111111111111110)) {
      bits ^= S2::kSwapMask;
    }
    *orientation = bits;
  }
  return face;
}

inline int S2CellId::GetCenterSiTi(int* psi, int* pti) const {
  // First we compute the discrete (i,j) coordinates of a leaf cell contained
  // within the given cell.  Given that cells are represented by the Hilbert
  // curve position corresponding at their center, it turns out that the cell
  // returned by ToFaceIJOrientation is always one of two leaf cells closest
  // to the center of the cell (unless the given cell is a leaf cell itself,
  // in which case there is only one possibility).
  //
  // Given a cell of size s >= 2 (i.e. not a leaf cell), and letting (imin,
  // jmin) be the coordinates of its lower left-hand corner, the leaf cell
  // returned by ToFaceIJOrientation() is either (imin + s/2, jmin + s/2)
  // (imin + s/2 - 1, jmin + s/2 - 1).  The first case is the one we want.
  // We can distinguish these two cases by looking at the low bit of "i" or
  // "j".  In the second case the low bit is one, unless s == 2 (i.e. the
  // level just above leaf cells) in which case the low bit is zero.
  //
  // In the code below, the expression ((i ^ (int(id_) >> 2)) & 1) is true
  // if we are in the second case described above.
  int i, j;
  int face = ToFaceIJOrientation(&i, &j, NULL);
  int delta = is_leaf() ? 1 : ((i ^ (static_cast<int>(id_) >> 2)) & 1) ? 2 : 0;

  // Note that (2 * {i,j} + delta) will never overflow a 32-bit integer.
  *psi = 2 * i + delta;
  *pti = 2 * j + delta;
  return face;
}

S2Point S2CellId::ToPointRaw() const {
  // This code would be slightly shorter if we called GetCenterUV(),
  // but this method is heavily used and it's 25% faster to include
  // the method inline.
  int si, ti;
  int face = GetCenterSiTi(&si, &ti);
  return S2::FaceUVtoXYZ(face,
                         S2::STtoUV((0.5 / kMaxSize) * si),
                         S2::STtoUV((0.5 / kMaxSize) * ti));
}

S2LatLng S2CellId::ToLatLng() const {
  return S2LatLng(ToPointRaw());
}

Vector2_d S2CellId::GetCenterST() const {
  int si, ti;
  GetCenterSiTi(&si, &ti);
  return Vector2_d((0.5 / kMaxSize) * si, (0.5 / kMaxSize) * ti);
}

Vector2_d S2CellId::GetCenterUV() const {
  int si, ti;
  GetCenterSiTi(&si, &ti);
  return Vector2_d(S2::STtoUV((0.5 / kMaxSize) * si),
                   S2::STtoUV((0.5 / kMaxSize) * ti));
}

S2CellId S2CellId::FromFaceIJWrap(int face, int i, int j) {
  // Convert i and j to the coordinates of a leaf cell just beyond the
  // boundary of this face.  This prevents 32-bit overflow in the case
  // of finding the neighbors of a face cell.
  i = max(-1, min(kMaxSize, i));
  j = max(-1, min(kMaxSize, j));

  // Find the (u,v) coordinates corresponding to the center of cell (i,j).
  // For our purposes it's sufficient to always use the linear projection
  // from (s,t) to (u,v): u=2*s-1 and v=2*t-1.
  static const double kScale = 1.0 / kMaxSize;
  double u = kScale * ((i << 1) + 1 - kMaxSize);
  double v = kScale * ((j << 1) + 1 - kMaxSize);

  // Find the leaf cell coordinates on the adjacent face, and convert
  // them to a cell id at the appropriate level.  We convert from (u,v)
  // back to (s,t) using s=0.5*(u+1), t=0.5*(v+1).
  face = S2::XYZtoFaceUV(S2::FaceUVtoXYZ(face, u, v), &u, &v);
  return FromFaceIJ(face, STtoIJ(0.5*(u+1)), STtoIJ(0.5*(v+1)));
}

inline S2CellId S2CellId::FromFaceIJSame(int face, int i, int j,
                                         bool same_face) {
  if (same_face)
    return S2CellId::FromFaceIJ(face, i, j);
  else
    return S2CellId::FromFaceIJWrap(face, i, j);
}

void S2CellId::GetEdgeNeighbors(S2CellId neighbors[4]) const {
  int i, j;
  int level = this->level();
  int size = GetSizeIJ(level);
  int face = ToFaceIJOrientation(&i, &j, NULL);

  // Edges 0, 1, 2, 3 are in the S, E, N, W directions.
  neighbors[0] = FromFaceIJSame(face, i, j - size, j - size >= 0)
                 .parent(level);
  neighbors[1] = FromFaceIJSame(face, i + size, j, i + size < kMaxSize)
                 .parent(level);
  neighbors[2] = FromFaceIJSame(face, i, j + size, j + size < kMaxSize)
                 .parent(level);
  neighbors[3] = FromFaceIJSame(face, i - size, j, i - size >= 0)
                 .parent(level);
}

void S2CellId::AppendVertexNeighbors(int level,
                                     vector<S2CellId>* output) const {
  // "level" must be strictly less than this cell's level so that we can
  // determine which vertex this cell is closest to.
  DCHECK_LT(level, this->level());
  int i, j;
  int face = ToFaceIJOrientation(&i, &j, NULL);

  // Determine the i- and j-offsets to the closest neighboring cell in each
  // direction.  This involves looking at the next bit of "i" and "j" to
  // determine which quadrant of this->parent(level) this cell lies in.
  int halfsize = GetSizeIJ(level + 1);
  int size = halfsize << 1;
  bool isame, jsame;
  int ioffset, joffset;
  if (i & halfsize) {
    ioffset = size;
    isame = (i + size) < kMaxSize;
  } else {
    ioffset = -size;
    isame = (i - size) >= 0;
  }
  if (j & halfsize) {
    joffset = size;
    jsame = (j + size) < kMaxSize;
  } else {
    joffset = -size;
    jsame = (j - size) >= 0;
  }

  output->push_back(parent(level));
  output->push_back(FromFaceIJSame(face, i + ioffset, j, isame).parent(level));
  output->push_back(FromFaceIJSame(face, i, j + joffset, jsame).parent(level));
  // If i- and j- edge neighbors are *both* on a different face, then this
  // vertex only has three neighbors (it is one of the 8 cube vertices).
  if (isame || jsame) {
    output->push_back(FromFaceIJSame(face, i + ioffset, j + joffset,
                                     isame && jsame).parent(level));
  }
}

void S2CellId::AppendAllNeighbors(int nbr_level,
                                  vector<S2CellId>* output) const {
  int i, j;
  int face = ToFaceIJOrientation(&i, &j, NULL);

  // Find the coordinates of the lower left-hand leaf cell.  We need to
  // normalize (i,j) to a known position within the cell because nbr_level
  // may be larger than this cell's level.
  int size = GetSizeIJ();
  i &= -size;
  j &= -size;

  int nbr_size = GetSizeIJ(nbr_level);
  DCHECK_LE(nbr_size, size);

  // We compute the N-S, E-W, and diagonal neighbors in one pass.
  // The loop test is at the end of the loop to avoid 32-bit overflow.
  for (int k = -nbr_size; ; k += nbr_size) {
    bool same_face;
    if (k < 0) {
      same_face = (j + k >= 0);
    } else if (k >= size) {
      same_face = (j + k < kMaxSize);
    } else {
      same_face = true;
      // North and South neighbors.
      output->push_back(FromFaceIJSame(face, i + k, j - nbr_size,
                                       j - size >= 0).parent(nbr_level));
      output->push_back(FromFaceIJSame(face, i + k, j + size,
                                       j + size < kMaxSize).parent(nbr_level));
    }
    // East, West, and Diagonal neighbors.
    output->push_back(FromFaceIJSame(face, i - nbr_size, j + k,
                                     same_face && i - size >= 0)
                      .parent(nbr_level));
    output->push_back(FromFaceIJSame(face, i + size, j + k,
                                     same_face && i + size < kMaxSize)
                      .parent(nbr_level));
    if (k >= size) break;
  }
}

inline char intToChar(int i) {
    return '0' + i;
}

inline uint64 charToInt(char c) {
    return static_cast<uint64>(c - '0');
}


S2CellId S2CellId::FromString(const string& str) {
    int face = charToInt(str[0]);
    int level = str.length() - 2;
    uint64 pos = 0;
    for (size_t i = 2; i < str.length(); ++i) {
        pos |= charToInt(str[i]) << (2 * (kMaxLevel - (i - 1)) + 1);
    }
    pos |= (uint64)1 << (2 * (kMaxLevel - level));
    return FromFacePosLevel(face, pos, level);
}

string S2CellId::ToString() const {
  if (!is_valid()) {
    return StringPrintf("Invalid: %016llx", id());
  }
  string out;
  out.reserve(2 + level());
  out.push_back(intToChar(face()));
  out.push_back('f');
  for (int current_level = 1; current_level <= level(); ++current_level) {
    out.push_back(intToChar(child_position(current_level)));
  }
  return out;
}

string S2CellId::slowToString() const {
  if (!is_valid()) {
    return StringPrintf("Invalid: %016llx", id());
  }
  string out = IntToString(face(), "%df");
  for (int current_level = 1; current_level <= level(); ++current_level) {
    out += IntToString(child_position(current_level), "%d");
  }
  return out;
}

ostream& operator<<(ostream& os, S2CellId const& id) {
  return os << id.ToString();
}

#ifdef OS_WINDOWS
template<> size_t stdext::hash_value<S2CellId>(const S2CellId &id) {
    return static_cast<size_t>(id.id() >> 32) + static_cast<size_t>(id.id());
}
#endif
