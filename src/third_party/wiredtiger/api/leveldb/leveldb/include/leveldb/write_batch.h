// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch holds a collection of updates to apply atomically to a DB.
//
// The updates are applied in the order in which they are added
// to the WriteBatch.  For example, the value of "key" will be "v3"
// after the following batch is written:
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// Multiple threads can invoke const methods on a WriteBatch without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same WriteBatch must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
#define STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_

#include "leveldb_wt_config.h"
#if defined(HAVE_ROCKSDB) && !defined(leveldb)
#define leveldb rocksdb
#endif

#include <string>
#include "status.h"

namespace leveldb {

class Slice;
#if HAVE_ROCKSDB
class ColumnFamilyHandle;
struct SliceParts;
#endif

class WriteBatch {
 public:
#ifdef HAVE_ROCKSDB
  explicit WriteBatch(size_t reserved_bytes = 0);
#else
  WriteBatch();
#endif
  ~WriteBatch();

  // Store the mapping "key->value" in the database.
  void Put(const Slice& key, const Slice& value);

  // If the database contains a mapping for "key", erase it.  Else do nothing.
  void Delete(const Slice& key);

  // Clear all updates buffered in this batch.
  void Clear();

#ifdef HAVE_ROCKSDB
  void Put(ColumnFamilyHandle* column_family, const Slice& key,
           const Slice& value);

  // Variant of Put() that gathers output like writev(2).  The key and value
  // that will be written to the database are concatentations of arrays of
  // slices.
  void Put(ColumnFamilyHandle* column_family, const SliceParts& key,
           const SliceParts& value);

  void Delete(ColumnFamilyHandle* column_family, const Slice& key);
#endif

  // Support for iterating over the contents of a batch.
  class Handler {
   public:
    virtual ~Handler();
#ifdef HAVE_ROCKSDB
    // default implementation will just call Put without column family for
    // backwards compatibility. If the column family is not default,
    // the function is noop
    virtual Status PutCF(uint32_t column_family_id, const Slice& key,
                         const Slice& value) {
      if (column_family_id == 0) {
        // Put() historically doesn't return status. We didn't want to be
        // backwards incompatible so we didn't change the return status
        // (this is a public API). We do an ordinary get and return Status::OK()
        Put(key, value);
        return Status::OK();
      }
      return Status::InvalidArgument(
          "non-default column family and PutCF not implemented");
    }
    // Merge and LogData are not pure virtual. Otherwise, we would break
    // existing clients of Handler on a source code level. The default
    // implementation of Merge simply throws a runtime exception.
    virtual Status MergeCF(uint32_t column_family_id, const Slice& key,
                           const Slice& value) {
      if (column_family_id == 0) {
        Merge(key, value);
        return Status::OK();
      }
      return Status::InvalidArgument(
          "non-default column family and MergeCF not implemented");
    }
    virtual void Merge(const Slice& key, const Slice& value);
    // The default implementation of LogData does nothing.
    virtual void LogData(const Slice& blob);
    virtual Status DeleteCF(uint32_t column_family_id, const Slice& key) {
      if (column_family_id == 0) {
        Delete(key);
        return Status::OK();
      }
      return Status::InvalidArgument(
          "non-default column family and DeleteCF not implemented");
    }
    // Continue is called by WriteBatch::Iterate. If it returns false,
    // iteration is halted. Otherwise, it continues iterating. The default
    // implementation always returns true.
    virtual bool Continue();
#endif
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };
  Status Iterate(Handler* handler) const;

#ifdef HAVE_ROCKSDB
  // Retrieve data size of the batch.
  size_t GetDataSize() const { return rep_.size(); }

  // Returns the number of updates in the batch
  int Count() const;
#endif

 private:
  friend class WriteBatchInternal;

  std::string rep_;  // See comment in write_batch.cc for the format of rep_

  // Intentionally copyable
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
