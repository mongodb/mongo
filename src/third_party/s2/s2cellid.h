// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2CELLID_H_
#define UTIL_GEOMETRY_S2CELLID_H_

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;

#include <string>
using std::string;

#include <vector>
using std::vector;

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/port.h"  // for HASH_NAMESPACE_DECLARATION_START
#include "s2.h"
#include "util/math/vector2.h"

class S2LatLng;

// An S2CellId is a 64-bit unsigned integer that uniquely identifies a
// cell in the S2 cell decomposition.  It has the following format:
//
//   id = [face][face_pos]
//
//   face:     a 3-bit number (range 0..5) encoding the cube face.
//
//   face_pos: a 61-bit number encoding the position of the center of this
//             cell along the Hilbert curve over this face (see the Wiki
//             pages for details).
//
// Sequentially increasing cell ids follow a continuous space-filling curve
// over the entire sphere.  They have the following properties:
//
//  - The id of a cell at level k consists of a 3-bit face number followed
//    by k bit pairs that recursively select one of the four children of
//    each cell.  The next bit is always 1, and all other bits are 0.
//    Therefore, the level of a cell is determined by the position of its
//    lowest-numbered bit that is turned on (for a cell at level k, this
//    position is 2 * (kMaxLevel - k).)
//
//  - The id of a parent cell is at the midpoint of the range of ids spanned
//    by its children (or by its descendants at any level).
//
// Leaf cells are often used to represent points on the unit sphere, and
// this class provides methods for converting directly between these two
// representations.  For cells that represent 2D regions rather than
// discrete point, it is better to use the S2Cell class.
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator.
class S2CellId {
 public:
  // Although only 60 bits are needed to represent the index of a leaf
  // cell, we need an extra bit in order to represent the position of
  // the center of the leaf cell along the Hilbert curve.
  static int const kFaceBits; // = 3;
  static int const kNumFaces; // = 6;
  static int const kMaxLevel;  // = S2::kMaxCellLevel;  // Valid levels: 0..kMaxLevel
  static int const kPosBits;  // = 2 * kMaxLevel + 1;
  static int const kMaxSize;  // = 1 << kMaxLevel;

  inline explicit S2CellId(uint64 id) : id_(id) {}

  // The default constructor returns an invalid cell id.
  inline S2CellId() : id_(0) {}
  inline static S2CellId None() { return S2CellId(); }

  // Returns an invalid cell id guaranteed to be larger than any
  // valid cell id.  Useful for creating indexes.
  inline static S2CellId Sentinel() { return S2CellId(~uint64(0)); }

  // Return a cell given its face (range 0..5), 61-bit Hilbert curve position
  // within that face, and level (range 0..kMaxLevel).  The given position
  // will be modified to correspond to the Hilbert curve position at the
  // center of the returned cell.  This is a static function rather than a
  // constructor in order to give names to the arguments.
  static S2CellId FromFacePosLevel(int face, uint64 pos, int level);

  // Return the leaf cell containing the given point (a direction
  // vector, not necessarily unit length).
  static S2CellId FromPoint(S2Point const& p);

  // Return the leaf cell containing the given normalized S2LatLng.
  static S2CellId FromLatLng(S2LatLng const& ll);

  // Return the direction vector corresponding to the center of the given
  // cell.  The vector returned by ToPointRaw is not necessarily unit length.
  S2Point ToPoint() const { return ToPointRaw().Normalize(); }
  S2Point ToPointRaw() const;

  // Return the center of the cell in (s,t) coordinates (see s2.h).
  Vector2_d GetCenterST() const;

  // Return the center of the cell in (u,v) coordinates (see s2.h).  Note that
  // the center of the cell is defined as the point at which it is recursively
  // subdivided into four children; in general, it is not at the midpoint of
  // the (u,v) rectangle covered by the cell.
  Vector2_d GetCenterUV() const;

  // Return the S2LatLng corresponding to the center of the given cell.
  S2LatLng ToLatLng() const;

  // The 64-bit unique identifier for this cell.
  inline uint64 id() const { return id_; }

  // Return true if id() represents a valid cell.
  inline bool is_valid() const;

  // Which cube face this cell belongs to, in the range 0..5.
  inline int face() const;

  // The position of the cell center along the Hilbert curve over this face,
  // in the range 0..(2**kPosBits-1).
  inline uint64 pos() const;

  // Return the subdivision level of the cell (range 0..kMaxLevel).
  int level() const;

  // Return the edge length of this cell in (i,j)-space.
  inline int GetSizeIJ() const;

  // Return the edge length of this cell in (s,t)-space.
  inline double GetSizeST() const;

  // Like the above, but return the size of cells at the given level.
  inline static int GetSizeIJ(int level);
  inline static double GetSizeST(int level);

  // Return true if this is a leaf cell (more efficient than checking
  // whether level() == kMaxLevel).
  inline bool is_leaf() const;

  // Return true if this is a top-level face cell (more efficient than
  // checking whether level() == 0).
  inline bool is_face() const;

  // Return the child position (0..3) of this cell's ancestor at the given
  // level, relative to its parent.  The argument should be in the range
  // 1..kMaxLevel.  For example, child_position(1) returns the position of
  // this cell's level-1 ancestor within its top-level face cell.
  inline int child_position(int level) const;

  // Methods that return the range of cell ids that are contained
  // within this cell (including itself).  The range is *inclusive*
  // (i.e. test using >= and <=) and the return values of both
  // methods are valid leaf cell ids.
  //
  // These methods should not be used for iteration.  If you want to
  // iterate through all the leaf cells, call child_begin(kMaxLevel) and
  // child_end(kMaxLevel) instead.
  //
  // It would in fact be error-prone to define a range_end() method,
  // because (range_max().id() + 1) is not always a valid cell id, and the
  // iterator would need to be tested using "<" rather that the usual "!=".
  inline S2CellId range_min() const;
  inline S2CellId range_max() const;

  // Return true if the given cell is contained within this one.
  inline bool contains(S2CellId const& other) const;

  // Return true if the given cell intersects this one.
  inline bool intersects(S2CellId const& other) const;

  // Return the cell at the previous level or at the given level (which must
  // be less than or equal to the current level).
  inline S2CellId parent() const;
  inline S2CellId parent(int level) const;

  // Return the immediate child of this cell at the given traversal order
  // position (in the range 0 to 3).  This cell must not be a leaf cell.
  inline S2CellId child(int position) const;

  // Iterator-style methods for traversing the immediate children of a cell or
  // all of the children at a given level (greater than or equal to the current
  // level).  Note that the end value is exclusive, just like standard STL
  // iterators, and may not even be a valid cell id.  You should iterate using
  // code like this:
  //
  //   for(S2CellId c = id.child_begin(); c != id.child_end(); c = c.next())
  //     ...
  //
  // The convention for advancing the iterator is "c = c.next()" rather
  // than "++c" to avoid possible confusion with incrementing the
  // underlying 64-bit cell id.
  inline S2CellId child_begin() const;
  inline S2CellId child_begin(int level) const;
  inline S2CellId child_end() const;
  inline S2CellId child_end(int level) const;

  // Return the next/previous cell at the same level along the Hilbert curve.
  // Works correctly when advancing from one face to the next, but
  // does *not* wrap around from the last face to the first or vice versa.
  inline S2CellId next() const;
  inline S2CellId prev() const;

  // This method advances or retreats the indicated number of steps along the
  // Hilbert curve at the current level, and returns the new position.  The
  // position is never advanced past End() or before Begin().
  S2CellId advance(int64 steps) const;

  // Like next() and prev(), but these methods wrap around from the last face
  // to the first and vice versa.  They should *not* be used for iteration in
  // conjunction with child_begin(), child_end(), Begin(), or End().  The
  // input must be a valid cell id.
  inline S2CellId next_wrap() const;
  inline S2CellId prev_wrap() const;

  // This method advances or retreats the indicated number of steps along the
  // Hilbert curve at the current level, and returns the new position.  The
  // position wraps between the first and last faces as necessary.  The input
  // must be a valid cell id.
  S2CellId advance_wrap(int64 steps) const;

  // Iterator-style methods for traversing all the cells along the Hilbert
  // curve at a given level (across all 6 faces of the cube).  Note that the
  // end value is exclusive (just like standard STL iterators), and is not a
  // valid cell id.
  inline static S2CellId Begin(int level);
  inline static S2CellId End(int level);

  // Methods to encode and decode cell ids to compact text strings suitable
  // for display or indexing.  Cells at lower levels (i.e. larger cells) are
  // encoded into fewer characters.  The maximum token length is 16.
  //
  // ToToken() returns a string by value for convenience; the compiler
  // does this without intermediate copying in most cases.
  //
  // These methods guarantee that FromToken(ToToken(x)) == x even when
  // "x" is an invalid cell id.  All tokens are alphanumeric strings.
  // FromToken() returns S2CellId::None() for malformed inputs.
  string ToToken() const;
  static S2CellId FromToken(string const& token);

  // Creates a debug human readable string. Used for << and available for direct
  // usage as well.
  string ToString() const;
  string toString() const { return ToString(); }
  string slowToString() const;
  static S2CellId FromString(const string& str);

  // Return the four cells that are adjacent across the cell's four edges.
  // Neighbors are returned in the order defined by S2Cell::GetEdge.  All
  // neighbors are guaranteed to be distinct.
  void GetEdgeNeighbors(S2CellId neighbors[4]) const;

  // Return the neighbors of closest vertex to this cell at the given level,
  // by appending them to "output".  Normally there are four neighbors, but
  // the closest vertex may only have three neighbors if it is one of the 8
  // cube vertices.
  //
  // Requires: level < this->level(), so that we can determine which vertex is
  // closest (in particular, level == kMaxLevel is not allowed).
  void AppendVertexNeighbors(int level, vector<S2CellId>* output) const;

  // Append all neighbors of this cell at the given level to "output".  Two
  // cells X and Y are neighbors if their boundaries intersect but their
  // interiors do not.  In particular, two cells that intersect at a single
  // point are neighbors.
  //
  // Requires: nbr_level >= this->level().  Note that for cells adjacent to a
  // face vertex, the same neighbor may be appended more than once.
  void AppendAllNeighbors(int nbr_level, vector<S2CellId>* output) const;

  /////////////////////////////////////////////////////////////////////
  // Low-level methods.

  // Return a leaf cell given its cube face (range 0..5) and
  // i- and j-coordinates (see s2.h).
  static S2CellId FromFaceIJ(int face, int i, int j);

  // Return the (face, i, j) coordinates for the leaf cell corresponding to
  // this cell id.  Since cells are represented by the Hilbert curve position
  // at the center of the cell, the returned (i,j) for non-leaf cells will be
  // a leaf cell adjacent to the cell center.  If "orientation" is non-NULL,
  // also return the Hilbert curve orientation for the current cell.
  int ToFaceIJOrientation(int* pi, int* pj, int* orientation) const;

  // Return the lowest-numbered bit that is on for this cell id, which is
  // equal to (uint64(1) << (2 * (kMaxLevel - level))).  So for example,
  // a.lsb() <= b.lsb() if and only if a.level() >= b.level(), but the
  // first test is more efficient.
#pragma warning(push)
#pragma warning( disable: 4146 )
  uint64 lsb() const { return id_ & -id_; }
#pragma warning(pop)

  // Return the lowest-numbered bit that is on for cells at the given level.
  inline static uint64 lsb_for_level(int level) {
    return uint64(1) << (2 * (kMaxLevel - level));
  }

 private:
  // This is the offset required to wrap around from the beginning of the
  // Hilbert curve to the end or vice versa; see next_wrap() and prev_wrap().
  static uint64 const kWrapOffset;  // = uint64(kNumFaces) << kPosBits;

  // Return the i- or j-index of the leaf cell containing the given
  // s- or t-value.  Values are clamped appropriately.
  inline static int STtoIJ(double s);

  // Return the (face, si, ti) coordinates of the center of the cell.  Note
  // that although (si,ti) coordinates span the range [0,2**31] in general,
  // the cell center coordinates are always in the range [1,2**31-1] and
  // therefore can be represented using a signed 32-bit integer.
  inline int GetCenterSiTi(int* psi, int* pti) const;

  // Given (i, j) coordinates that may be out of bounds, normalize them by
  // returning the corresponding neighbor cell on an adjacent face.
  static S2CellId FromFaceIJWrap(int face, int i, int j);

  // Inline helper function that calls FromFaceIJ if "same_face" is true,
  // or FromFaceIJWrap if "same_face" is false.
  inline static S2CellId FromFaceIJSame(int face, int i, int j, bool same_face);

  uint64 id_;
} PACKED;  // Necessary so that structures containing S2CellId's can be PACKED.
DECLARE_POD(S2CellId);

inline bool operator==(S2CellId const& x, S2CellId const& y) {
  return x.id() == y.id();
}

inline bool operator!=(S2CellId const& x, S2CellId const& y) {
  return x.id() != y.id();
}

inline bool operator<(S2CellId const& x, S2CellId const& y) {
  return x.id() < y.id();
}

inline bool operator>(S2CellId const& x, S2CellId const& y) {
  return x.id() > y.id();
}

inline bool operator<=(S2CellId const& x, S2CellId const& y) {
  return x.id() <= y.id();
}

inline bool operator>=(S2CellId const& x, S2CellId const& y) {
  return x.id() >= y.id();
}

inline bool S2CellId::is_valid() const {
  return (face() < kNumFaces && (lsb() & GG_ULONGLONG(0x1555555555555555)));
}

inline int S2CellId::face() const {
  return id_ >> kPosBits;
}

inline uint64 S2CellId::pos() const {
  return id_ & (~uint64(0) >> kFaceBits);
}

inline int S2CellId::GetSizeIJ() const {
  return GetSizeIJ(level());
}

inline double S2CellId::GetSizeST() const {
  return GetSizeST(level());
}

inline int S2CellId::GetSizeIJ(int level) {
  return 1 << (kMaxLevel - level);
}

inline double S2CellId::GetSizeST(int level) {
  // Floating-point multiplication is much faster than division.
  return GetSizeIJ(level) * (1.0 / kMaxSize);
}

inline bool S2CellId::is_leaf() const {
  return int(id_) & 1;
}

inline bool S2CellId::is_face() const {
  return (id_ & (lsb_for_level(0) - 1)) == 0;
}

inline int S2CellId::child_position(int level) const {
  DCHECK(is_valid());
  return static_cast<int>(id_ >> (2 * (kMaxLevel - level) + 1)) & 3;
}

inline S2CellId S2CellId::range_min() const {
  return S2CellId(id_ - (lsb() - 1));
}

inline S2CellId S2CellId::range_max() const {
  return S2CellId(id_ + (lsb() - 1));
}

inline bool S2CellId::contains(S2CellId const& other) const {
  DCHECK(is_valid());
  DCHECK(other.is_valid());
  return other >= range_min() && other <= range_max();
}

inline bool S2CellId::intersects(S2CellId const& other) const {
  DCHECK(is_valid());
  DCHECK(other.is_valid());
  return other.range_min() <= range_max() && other.range_max() >= range_min();
}

inline S2CellId S2CellId::parent(int level) const {
  DCHECK(is_valid());
  DCHECK_GE(level, 0);
  DCHECK_LE(level, this->level());
  uint64 new_lsb = lsb_for_level(level);
#pragma warning(push)
#pragma warning( disable: 4146 )
  return S2CellId((id_ & -new_lsb) | new_lsb);
#pragma warning(pop)
}

inline S2CellId S2CellId::parent() const {
  DCHECK(is_valid());
  DCHECK(!is_face());
  uint64 new_lsb = lsb() << 2;
#pragma warning(push)
#pragma warning( disable: 4146 )
  return S2CellId((id_ & -new_lsb) | new_lsb);
#pragma warning(pop)
}

inline S2CellId S2CellId::child(int position) const {
  DCHECK(is_valid());
  DCHECK(!is_leaf());
  // To change the level, we need to move the least-significant bit two
  // positions downward.  We do this by subtracting (4 * new_lsb) and adding
  // new_lsb.  Then to advance to the given child cell, we add
  // (2 * position * new_lsb).
  uint64 new_lsb = lsb() >> 2;
  return S2CellId(id_ + (2 * position + 1 - 4) * new_lsb);
}

inline S2CellId S2CellId::child_begin() const {
  DCHECK(is_valid());
  DCHECK(!is_leaf());
  uint64 old_lsb = lsb();
  return S2CellId(id_ - old_lsb + (old_lsb >> 2));
}

inline S2CellId S2CellId::child_begin(int level) const {
  DCHECK(is_valid());
  DCHECK_GE(level, this->level());
  DCHECK_LE(level, kMaxLevel);
  return S2CellId(id_ - lsb() + lsb_for_level(level));
}

inline S2CellId S2CellId::child_end() const {
  DCHECK(is_valid());
  DCHECK(!is_leaf());
  uint64 old_lsb = lsb();
  return S2CellId(id_ + old_lsb + (old_lsb >> 2));
}

inline S2CellId S2CellId::child_end(int level) const {
  DCHECK(is_valid());
  DCHECK_GE(level, this->level());
  DCHECK_LE(level, kMaxLevel);
  return S2CellId(id_ + lsb() + lsb_for_level(level));
}

inline S2CellId S2CellId::next() const {
  return S2CellId(id_ + (lsb() << 1));
}

inline S2CellId S2CellId::prev() const {
  return S2CellId(id_ - (lsb() << 1));
}

inline S2CellId S2CellId::next_wrap() const {
  DCHECK(is_valid());
  S2CellId n = next();
  if (n.id_ < kWrapOffset) return n;
  return S2CellId(n.id_ - kWrapOffset);
}

inline S2CellId S2CellId::prev_wrap() const {
  DCHECK(is_valid());
  S2CellId p = prev();
  if (p.id_ < kWrapOffset) return p;
  return S2CellId(p.id_ + kWrapOffset);
}

inline S2CellId S2CellId::Begin(int level) {
  return FromFacePosLevel(0, 0, 0).child_begin(level);
}

inline S2CellId S2CellId::End(int level) {
  return FromFacePosLevel(5, 0, 0).child_end(level);
}

ostream& operator<<(ostream& os, S2CellId const& id);

HASH_NAMESPACE_START
template<> struct hash<S2CellId> {
  size_t operator()(S2CellId const& id) const {
    return static_cast<size_t>(id.id() >> 32) + static_cast<size_t>(id.id());
  }
};
HASH_NAMESPACE_END

#endif  // UTIL_GEOMETRY_S2CELLID_H_
