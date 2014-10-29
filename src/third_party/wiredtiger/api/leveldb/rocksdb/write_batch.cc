//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring
//    kTypeMerge varstring varstring
//    kTypeDeletion varstring
//    kTypeColumnFamilyValue varint32 varstring varstring
//    kTypeColumnFamilyMerge varint32 varstring varstring
//    kTypeColumnFamilyDeletion varint32 varstring varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

#include "leveldb_wt.h"

#include "db/write_batch_internal.h"

#include <stdexcept>

namespace rocksdb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
static const size_t kHeader = 12;

WriteBatch::WriteBatch(size_t reserved_bytes) {
  rep_.reserve((reserved_bytes > kHeader) ? reserved_bytes : kHeader);
  Clear();
}

WriteBatch::~WriteBatch() { }

WriteBatch::Handler::~Handler() { }

void WriteBatch::Handler::Put(const Slice& key, const Slice& value) {
  // you need to either implement Put or PutCF
  throw std::runtime_error("Handler::Put not implemented!");
}

#ifdef NOT_YET
void WriteBatch::Handler::Merge(const Slice& key, const Slice& value) {
  throw std::runtime_error("Handler::Merge not implemented!");
}
#endif

void WriteBatch::Handler::Delete(const Slice& key) {
  // you need to either implement Delete or DeleteCF
  throw std::runtime_error("Handler::Delete not implemented!");
}

#ifdef NOT_YET
void WriteBatch::Handler::LogData(const Slice& blob) {
  // If the user has not specified something to do with blobs, then we ignore
  // them.
}
#endif

bool WriteBatch::Handler::Continue() {
  return true;
}

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

int WriteBatch::Count() const {
  return WriteBatchInternal::Count(this);
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value, blob;
  int found = 0;
  Status s;
  while (s.ok() && !input.empty() && handler->Continue()) {
    char tag = input[0];
    input.remove_prefix(1);
    uint32_t column_family = 0;  // default
    switch (tag) {
      case kTypeColumnFamilyValue:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Put");
        }
      // intentional fallthrough
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          s = handler->PutCF(column_family, key, value);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeColumnFamilyDeletion:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Delete");
        }
      // intentional fallthrough
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          s = handler->DeleteCF(column_family, key);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      case kTypeColumnFamilyMerge:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Merge");
        }
      // intentional fallthrough
      case kTypeMerge:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          s = handler->MergeCF(column_family, key, value);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Merge");
        }
        break;
      case kTypeLogData:
        if (GetLengthPrefixedSlice(&input, &blob)) {
          handler->LogData(blob);
        } else {
          return Status::Corruption("bad WriteBatch Blob");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (!s.ok()) {
    return s;
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}

int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

#ifdef NOT_YET
SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}
#endif

void WriteBatchInternal::Put(WriteBatch* b, uint32_t column_family_id,
                             const Slice& key, const Slice& value) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&b->rep_, key);
  PutLengthPrefixedSlice(&b->rep_, value);
}

namespace {
inline uint32_t GetColumnFamilyID(ColumnFamilyHandle* column_family) {
  uint32_t column_family_id = 0;
  if (column_family != NULL) {
    ColumnFamilyHandleImpl *cfh = reinterpret_cast<ColumnFamilyHandleImpl*>(column_family);
    column_family_id = cfh->GetID();
  }
  return column_family_id;
}
}  // namespace

void WriteBatch::Put(ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value) {
  WriteBatchInternal::Put(this, GetColumnFamilyID(column_family), key, value);
}

void WriteBatchInternal::Put(WriteBatch* b, uint32_t column_family_id,
                             const SliceParts& key, const SliceParts& value) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSliceParts(&b->rep_, key);
  PutLengthPrefixedSliceParts(&b->rep_, value);
}

void WriteBatch::Put(ColumnFamilyHandle* column_family, const SliceParts& key,
                     const SliceParts& value) {
  WriteBatchInternal::Put(this, GetColumnFamilyID(column_family), key, value);
}

void WriteBatchInternal::Delete(WriteBatch* b, uint32_t column_family_id,
                                const Slice& key) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeDeletion));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyDeletion));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&b->rep_, key);
}

void WriteBatch::Delete(ColumnFamilyHandle* column_family, const Slice& key) {
  WriteBatchInternal::Delete(this, GetColumnFamilyID(column_family), key);
}

#ifdef NOT_YET
void WriteBatchInternal::Merge(WriteBatch* b, uint32_t column_family_id,
                               const Slice& key, const Slice& value) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeMerge));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyMerge));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&b->rep_, key);
  PutLengthPrefixedSlice(&b->rep_, value);
}

void WriteBatch::Merge(ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value) {
  WriteBatchInternal::Merge(this, GetColumnFamilyID(column_family), key, value);
}

void WriteBatch::PutLogData(const Slice& blob) {
  rep_.push_back(static_cast<char>(kTypeLogData));
  PutLengthPrefixedSlice(&rep_, blob);
}
#endif

void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace rocksdb
