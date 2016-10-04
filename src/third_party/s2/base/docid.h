// Copyright 2001 and onwards Google Inc.

#ifndef BASE_DOCID_H_
#define BASE_DOCID_H_

#include "base/definer.h"
#include <assert.h>

// sys/types.h is only needed by this file if !defined(NDEBUG),
// but including it conditionally can cause hard-to-track-down
// compilation failures that only happen when doing optimized builds.
// Let's include it unconditionally to reduce the number of such breakages
// we need to track down.
// Note that uint is defined by sys/types.h only if _GNU_SOURCE or its ilk is
// defined, and that mysql.h seems to define _GNU_SOURCE, so the problem
// may only happen if sys/types.h is included after mysql.h.
// We now define uint in base/port.h if needed, and in a way that won't clash.
#include <sys/types.h>       // for size_t

#include "base/basictypes.h"

#define DOCID_FORMAT "%llu"
#define DOCID32BIT_FORMAT "%u"

// The following help ensure type safety for docids.  Use them when
// you want to printf/scanf a docid.  Note that even so, when the
// type of docid changes you'll need to change all the format args
// for these printf/scanf statements by hand (to be %qu, say).
//
// We do two kinds of docids to facilitate the transition from 32-bit
// docids to 64-bit docids. We need both in the transition period
// as the base index will use 32 bit docids and the incremental index
// will use 64-bit docids.

#if defined NDEBUG
typedef uint64 DocId;
const DocId kMaxDocId = DocId(GG_ULONGLONG(0xFFFFFFFFFFFFFFFF));

static inline uint64 DocidForPrintf(const DocId& d)   { return d; }
static inline uint64* DocidForScanf(DocId* d)         { return d; }
// Used with the SparseBitmap class, primarily
static inline uint64 DocidForBitmap(const DocId& d)  { return d; }
// Used with the docservercache stuff, for odd but reasonable reasons
static inline uint64 DocidForFingerprinting(const DocId& d)  { return d; }
// Used with the protocol-buffer stuff
static inline uint64 DocidForProtocolBuffer(const DocId& d)  { return d; }
// Used during anchor processing
static inline uint64 DocidForAnchorPosition(const DocId& d) { return d; }
// Used for checkpointing
static inline uint64 DocidForCheckpointing(const DocId& d) { return d; }
// Used for approximate dups detection
static inline uint64 DocidForApproxDups(const DocId& d) { return d; }
// Used for SWIG
static inline uint64* DocidForSWIG(DocId* d)         { return d; }

// Used for miscellaneous arithmetic.
static inline uint64 DocIdAsNumber(const DocId& d) { return d; }

// ------------------------------------------------------------------
// 32 bit docids
// ------------------------------------------------------------------
typedef uint32 DocId32Bit;
const DocId32Bit kMaxDocId32Bit = static_cast<DocId32Bit>(0xFFFFFFFFul);
static inline uint32 Docid32BitForPrintf(const DocId32Bit& d) { return d; }
static inline uint32* Docid32BitForScanf(DocId32Bit* d) { return d; }
static inline uint32 Docid32BitForBitmap(const DocId32Bit& d)  { return d; }
static inline uint32 Docid32BitForFingerprinting(const DocId32Bit& d) {
  return d;
}
static inline uint32 Docid32BitForEncryption(const DocId32Bit &d) {
  return d;
}
static inline uint32 Docid32BitForProtocolBuffer(const DocId32Bit& d) {
  return d;
}
static inline uint32 Docid32BitForAnchorPosition(const DocId32Bit& d) {
  return d;
}
static inline uint32 Docid32BitForCheckpointing(const DocId32Bit& d) {
  return d;
}
static inline uint32 Docid32BitForApproxDups(const DocId32Bit& d) { return d; }

static inline uint32 DocId32BitAsNumber(const DocId32Bit& d) { return d; }

// return true if the value cannot fit in DocId32Bit
static inline bool Is64BitDocId(const DocId& d) { return d > kMaxDocId32Bit; }

#else
// In debug mode, we make docid its own type so we can be sure of
// type-safety.  In particular, we're concerned with people who treat
// docids as if they were signed, or who assume they're 32 bits long.
// We don't define this always because it results in 14% bigger executables.
//
// DocId is thread-compatible.
#define BINPRED_(pred)   \
  bool operator pred (const DocId& x) const { return docid_ pred x.docid_; }
class DocId {
 protected:
  typedef uint64 value_type;
  value_type docid_;

 public:
  DocId()                               { docid_ = 0; }
  explicit DocId(value_type docid)      { docid_ = docid; }
  value_type docid() const              { return docid_; }   // ONLY use here
  value_type* docidptr()                { return &docid_; }  // ie in this file
  DocId& operator=(const DocId& x) {
    docid_ = x.docid_;
    return *this;
  }
  // These two are required for delta encoding
  value_type operator+(const DocId& x) const { return docid_ + x.docid_; }
  value_type operator-(const DocId& x) const { return docid_ - x.docid_; }

  // needed in incrementalpr.cc
  DocId operator+(uint64 x) const      { return DocId(docid_ + x); }
  DocId operator-(uint64 x) const      { return DocId(docid_ - x); }
  // Required for hashing, typically
  value_type operator%(value_type m) const   { return docid_ % m; }
  // Required for sortedrepptrs.cc and partialindexreader.cc
  DocId& operator+=(uint64 x) {
    docid_ += x;
    return *this;
  }
  // Required for moreoverreposwriter.cc & sortpageranks.cc
  DocId& operator++() {
    ++docid_;
    return *this;
  }
  DocId& operator--() {
    --docid_;
    return *this;
  }
  // Required by dup_docid_categorizer.cc and some other twiddlers, who needs
  // to convert 64-bit doc id into 32-bit category id. Now the only intended use
  // of this operator is to mask off the top bits of a docid.
  value_type operator&(value_type x) const { return docid_ & x; }
  // Comparators
  BINPRED_(==) BINPRED_(!=)  BINPRED_(<) BINPRED_(>)  BINPRED_(<=) BINPRED_(>=)
};
DECLARE_POD(DocId);
#undef BINPRED_

// Required for anchorbucket.cc
inline double operator/(double a, const DocId& b) {
  return a / static_cast<double>(b.docid()); }
// Required for restrict-tool.cc
inline uint64 operator/(const DocId& a, uint64 b) { return a.docid() / b; }
// Required for index-request.cc
inline double operator*(const DocId& a, double b) {
  return static_cast<double>(a.docid()) * b; }
// Required for linksreader.cc
inline uint64 operator*(const DocId& a, uint64 b) { return a.docid() * b; }
// Required for indexdefs.h (really should be HitPosition, not uint64)
inline uint64 operator+(uint64 a, const DocId& b) { return a + b.docid(); }

// Required for hashing docids.  docservercache.cc needs to fingerprint them.
#if !defined __SGI_STL_HASH_FUN_H       // taken from stl_decl.h
HASH_NAMESPACE_DECLARATION_START
template <class Key> class hash;
HASH_NAMESPACE_DECLARATION_END
#endif
HASH_NAMESPACE_DECLARATION_START
template<> class hash<DocId> {
  size_t operator()(const DocId& d) const {
    return static_cast<size_t>(d.docid());
  }
};
HASH_NAMESPACE_DECLARATION_END

const DocId kMaxDocId = DocId(GG_ULONGLONG(0xFFFFFFFFFFFFFFFF));

// These are defined in non-debug mode too; they're always-on type-safety
static inline uint64 DocidForPrintf(const DocId& d)  { return d.docid(); }
static inline uint64* DocidForScanf(DocId* d)        { return d->docidptr(); }
static inline uint64 DocidForBitmap(const DocId& d)  { return d.docid(); }
static inline uint64 DocidForFingerprinting(const DocId& d)
    { return d.docid(); }
static inline uint64 DocidForProtocolBuffer(const DocId& d)
    { return d.docid(); }
static inline uint64 DocidForAnchorPosition(const DocId& d)
    { return d.docid(); }
static inline uint64 DocidForCheckpointing(const DocId& d)
    { return d.docid(); }
static inline uint64 DocidForApproxDups(const DocId& d) { return d.docid(); }
static inline uint64 DocIdAsNumber(const DocId& d) { return d.docid(); }
static inline uint64* DocidForSWIG(DocId* d)        { return d->docidptr(); }

// ---------------------------------------------------------------------
// 32 bit docids
//
// ---------------------------------------------------------------------

#define BINPRED32Bit_(pred)   \
  bool operator pred(const DocId32Bit& x) const { \
    return docid_ pred x.docid_; \
  }

class DocId32Bit {
 protected:
  typedef uint32 value_type;
  value_type docid_;

 public:
  DocId32Bit()                          { docid_ = 0; }
  explicit DocId32Bit(value_type docid) { docid_ = docid; }
  explicit DocId32Bit(DocId docid) {
    // we can't use kMaxDocId32Bit as it is not yet defined
    assert(DocIdAsNumber(docid) <= 0xFFFFFFFFul);
    docid_ = static_cast<uint32> (DocIdAsNumber(docid));
  }
  value_type docid() const              { return docid_; }   // ONLY use here
  value_type* docidptr()                { return &docid_; }  // ie in this file
  DocId32Bit& operator=(const DocId32Bit& x) {
    docid_ = x.docid_;
    return *this;
  }
  // These two are required for delta encoding
  value_type operator+(const DocId32Bit& x) const { return docid_ + x.docid_; }
  value_type operator-(const DocId32Bit& x) const { return docid_ - x.docid_; }

  // needed in incrementalpr.cc
  DocId32Bit operator+(uint32 x) const      { return DocId32Bit(docid_ + x); }
  DocId32Bit operator-(uint32 x) const      { return DocId32Bit(docid_ - x); }
  // Required for hashing, typically
  value_type operator%(value_type m) const   { return docid_ % m; }
  // Required for sortedrepptrs.cc and partialindexreader.cc
  DocId32Bit& operator+=(uint32 x) {
    docid_ += x;
    return *this;
  }
  // Required for moreoverreposwriter.cc & sortpageranks.cc
  DocId32Bit& operator++() {
    ++docid_;
    return *this;
  }
  DocId32Bit& operator--() {
    --docid_;
    return *this;
  }
  // Comparators
  BINPRED32Bit_(==) BINPRED32Bit_(!=)  BINPRED32Bit_(<) BINPRED32Bit_(>)
  BINPRED32Bit_(<=) BINPRED32Bit_(>=)
};
DECLARE_POD(DocId32Bit);
#undef BINPRED32Bit_

const DocId32Bit kMaxDocId32Bit = DocId32Bit(0xFFFFFFFFul);
static inline uint32 Docid32BitForBitmap(const DocId32Bit& d)
    { return d.docid(); }
static inline uint32 Docid32BitForPrintf(const DocId32Bit& d)
    { return d.docid(); }
static inline uint32* Docid32BitForScanf(DocId32Bit* d)
    { return d->docidptr(); }
static inline uint32 Docid32BitForFingerprinting(const DocId32Bit& d)
    { return d.docid(); }
static inline uint32 Docid32BitForEncryption(const DocId32Bit& d)
    { return d.docid(); }
static inline int32 Docid32BitForProtocolBuffer(const DocId32Bit& d)
    { return d.docid(); }
static inline uint32 Docid32BitForAnchorPosition(const DocId32Bit& d)
    { return d.docid(); }
static inline uint32 Docid32BitForCheckpointing(const DocId32Bit& d)
    { return d.docid(); }
static inline uint32 Docid32BitForApproxDups(const DocId32Bit& d)
    { return d.docid(); }
static inline uint32 DocId32BitAsNumber(const DocId32Bit& d)
    { return d.docid(); }

static inline bool Is64BitDocId(const DocId& d)
    { return d.docid() > DocId32BitAsNumber(kMaxDocId32Bit); }

inline double operator/(double a, const DocId32Bit& b)
    { return a / b.docid(); }
inline int operator/(const DocId32Bit& a, int b)  { return a.docid() / b; }
inline double operator*(const DocId32Bit& a, double b)
    { return a.docid() * b; }
inline uint64 operator*(const DocId32Bit& a, uint64 b)
    { return a.docid() * b; }
inline uint32 operator+(uint32 a, const DocId32Bit& b)
    { return a + b.docid(); }

HASH_NAMESPACE_DECLARATION_START
template<> struct hash<DocId32Bit> {
  size_t operator()(const DocId32Bit& d) const { return d.docid(); }
};
HASH_NAMESPACE_DECLARATION_END

#endif

// some definitions for 64 bit docids
const DocId kIllegalDocId = DocId(0);
const DocId kInitialDocId = DocId(1);
inline DocId NextDocIdOnShard(DocId d, int shard, int num_shards) {
  return DocId((((DocIdAsNumber(d) + num_shards - 1 - shard) /
                 num_shards) * num_shards) + shard);
}

// some definitions for 32 bit docids
const DocId32Bit kIllegalDocId32Bit = DocId32Bit(0);
const DocId32Bit kInitialDocId32Bit = DocId32Bit(1);

// Type for index-localized docids (normal docid divided by # of
// shards).  This type is signed because 0 is a valid local docid (for
// shard > 0), and we need an illegal value.
typedef int32 LocalDocId;
const LocalDocId kMaxLocalDocId = static_cast<LocalDocId>(0x7FFFFFFF);
const LocalDocId kIllegalLocalDocId = static_cast<LocalDocId>(~0);

// Conversion between local and global docids.
// REQUIRES: the arguments should be neither Illegal nor Max.
inline LocalDocId GlobalToLocalDocId(DocId d, int /*shard*/, int num_shards) {
  assert(d > kIllegalDocId);
  assert(d < kMaxDocId);
  return static_cast<LocalDocId>(DocIdAsNumber(d) / num_shards);
}
inline DocId LocalToGlobalDocId(LocalDocId d, int shard, int num_shards) {
  assert(d > kIllegalLocalDocId);
  assert(d < kMaxLocalDocId);
  return DocId((static_cast<uint32>(d)*num_shards) + shard);
}

// Equivalent to GlobalToLocalDocId(NextDocIdOnShard(d, sh, ns), sh, ns)
inline LocalDocId NextLocalDocIdOnShard(DocId d, int shard, int num_shards) {
  return GlobalToLocalDocId(NextDocIdOnShard(d, shard, num_shards),
                            shard, num_shards);
}

inline DocId DocIdFromUrlfp(Fprint urlfp) {
  return DocId(urlfp);
}

// DocVersionId

// For real time cache.  A higher value means more recent.
typedef uint32 DocVersionIdVal;
const DocVersionIdVal kIllegalDocVersionId = static_cast<DocVersionIdVal>(0);

#endif  // BASE_DOCID_H_
