// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_DB_H_
#define STORAGE_LEVELDB_INCLUDE_DB_H_

#include "leveldb_wt_config.h"
#if defined(HAVE_ROCKSDB) && !defined(leveldb)
#define leveldb rocksdb
#endif

#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "iterator.h"
#include "options.h"
#include "write_batch.h"
#ifdef HAVE_HYPERLEVELDB
#include "replay_iterator.h"
#endif

namespace leveldb {

// Update Makefile if you change these
static const int kMajorVersion = 1;
static const int kMinorVersion = 17;

struct ReadOptions;
struct WriteOptions;
class WriteBatch;

#ifdef HAVE_ROCKSDB
struct FlushOptions;
class ColumnFamilyHandle {
 public:
  virtual ~ColumnFamilyHandle() {}
};
extern const std::string kDefaultColumnFamilyName;
 
struct ColumnFamilyDescriptor {
  std::string name;
  ColumnFamilyOptions options;
  ColumnFamilyDescriptor()
      : name(kDefaultColumnFamilyName), options(ColumnFamilyOptions()) {}
  ColumnFamilyDescriptor(const std::string& _name,
                         const ColumnFamilyOptions& _options)
      : name(_name), options(_options) {}
};
#endif

// Abstract handle to particular state of a DB.
// A Snapshot is an immutable object and can therefore be safely
// accessed from multiple threads without any external synchronization.
class Snapshot {
 protected:
  virtual ~Snapshot();
};

// A range of keys
struct Range {
  Slice start;          // Included in the range
  Slice limit;          // Not included in the range

  Range() { }
  Range(const Slice& s, const Slice& l) : start(s), limit(l) { }
};

#if HAVE_BASHOLEVELDB
// Abstract holder for a DB value.
// This allows callers to manage their own value buffers and have
// DB values copied directly into those buffers.
class Value {
 public:
  virtual Value& assign(const char* data, size_t size) = 0;

 protected:
  virtual ~Value();
};
#endif

// A DB is a persistent ordered map from keys to values.
// A DB is safe for concurrent access from multiple threads without
// any external synchronization.
class DB {
 public:
  // Open the database with the specified "name".
  // Stores a pointer to a heap-allocated database in *dbptr and returns
  // OK on success.
  // Stores NULL in *dbptr and returns a non-OK status on error.
  // Caller should delete *dbptr when it is no longer needed.
  static Status Open(const Options& options,
                     const std::string& name,
                     DB** dbptr);

#ifdef HAVE_ROCKSDB
 // Open DB with column families.
 // db_options specify database specific options
 // column_families is the vector of all column families in the databse,
 // containing column family name and options. You need to open ALL column
 // families in the database. To get the list of column families, you can use
 // ListColumnFamilies(). Also, you can open only a subset of column families
 // for read-only access.
 // The default column family name is 'default' and it's stored
 // in rocksdb::kDefaultColumnFamilyName.
 // If everything is OK, handles will on return be the same size
 // as column_families --- handles[i] will be a handle that you
 // will use to operate on column family column_family[i]
 static Status Open(const Options& db_options, const std::string& name,
                    const std::vector<ColumnFamilyDescriptor>& column_families,
                    std::vector<ColumnFamilyHandle*>* handles, DB** dbptr);

 // ListColumnFamilies will open the DB specified by argument name
 // and return the list of all column families in that DB
 // through column_families argument. The ordering of
 // column families in column_families is unspecified.
 static Status ListColumnFamilies(const Options& db_options,
                                  const std::string& name,
                                  std::vector<std::string>* column_families);

  // Create a column_family and return the handle of column family
  // through the argument handle.
  virtual Status CreateColumnFamily(const Options& options,
                                    const std::string& column_family_name,
                                    ColumnFamilyHandle** handle) = 0;

  // Drop a column family specified by column_family handle. This call
  // only records a drop record in the manifest and prevents the column
  // family from flushing and compacting.
  virtual Status DropColumnFamily(ColumnFamilyHandle* column_family) = 0;

  // Set the database entry for "key" to "value".
  // Returns OK on success, and a non-OK status on error.
  // Note: consider setting options.sync = true.
  virtual Status Put(const WriteOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value) = 0;

  // Remove the database entry (if any) for "key".  Returns OK on
  // success, and a non-OK status on error.  It is not an error if "key"
  // did not exist in the database.
  // Note: consider setting options.sync = true.
  virtual Status Delete(const WriteOptions& options,
                        ColumnFamilyHandle* column_family,
                        const Slice& key) = 0;

  // Merge the database entry for "key" with "value".  Returns OK on success,
  // and a non-OK status on error. The semantics of this operation is
  // determined by the user provided merge_operator when opening DB.
  // Note: consider setting options.sync = true.
  virtual Status Merge(const WriteOptions& options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value) = 0;

  // May return some other Status on an error.
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     std::string* value) = 0;

  // If keys[i] does not exist in the database, then the i'th returned
  // status will be one for which Status::IsNotFound() is true, and
  // (*values)[i] will be set to some arbitrary value (often ""). Otherwise,
  // the i'th returned status will have Status::ok() true, and (*values)[i]
  // will store the value associated with keys[i].
  //
  // (*values) will always be resized to be the same size as (keys).
  // Similarly, the number of returned statuses will be the number of keys.
  // Note: keys will not be "de-duplicated". Duplicate keys will return
  // duplicate values in order.
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys, std::vector<std::string>* values) = 0;

  // If the key definitely does not exist in the database, then this method
  // returns false, else true. If the caller wants to obtain value when the key
  // is found in memory, a bool for 'value_found' must be passed. 'value_found'
  // will be true on return if value has been set properly.
  // This check is potentially lighter-weight than invoking DB::Get(). One way
  // to make this lighter weight is to avoid doing any IOs.
  // Default implementation here returns true and sets 'value_found' to false
  virtual bool KeyMayExist(const ReadOptions&,
                           ColumnFamilyHandle*, const Slice&,
                           std::string*, bool* value_found = NULL) {
    if (value_found != NULL) {
      *value_found = false;
    }
    return true;
  }

  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family) = 0;

  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value) = 0;

  // Flush all mem-table data.
  virtual Status Flush(const FlushOptions& options,
                       ColumnFamilyHandle* column_family) = 0;
#endif

  DB() { }
  virtual ~DB();

  // Set the database entry for "key" to "value".  Returns OK on success,
  // and a non-OK status on error.
  // Note: consider setting options.sync = true.
  virtual Status Put(const WriteOptions& options,
                     const Slice& key,
                     const Slice& value) = 0;

  // Remove the database entry (if any) for "key".  Returns OK on
  // success, and a non-OK status on error.  It is not an error if "key"
  // did not exist in the database.
  // Note: consider setting options.sync = true.
  virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;

  // Apply the specified updates to the database.
  // Returns OK on success, non-OK on failure.
  // Note: consider setting options.sync = true.
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

  // If the database contains an entry for "key" store the
  // corresponding value in *value and return OK.
  //
  // If there is no entry for "key" leave *value unchanged and return
  // a status for which Status::IsNotFound() returns true.
  //
  // May return some other Status on an error.
  virtual Status Get(const ReadOptions& options,
                     const Slice& key, std::string* value) = 0;
#if HAVE_BASHOLEVELDB
  virtual Status Get(const ReadOptions& options,
                     const Slice& key, Value* value) = 0;
#endif

  // Return a heap-allocated iterator over the contents of the database.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  //
  // Caller should delete the iterator when it is no longer needed.
  // The returned iterator should be deleted before this db is deleted.
  virtual Iterator* NewIterator(const ReadOptions& options) = 0;

  // Return a handle to the current DB state.  Iterators created with
  // this handle will all observe a stable snapshot of the current DB
  // state.  The caller must call ReleaseSnapshot(result) when the
  // snapshot is no longer needed.
  virtual const Snapshot* GetSnapshot() = 0;

  // Release a previously acquired snapshot.  The caller must not
  // use "snapshot" after this call.
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // DB implementations can export properties about their state
  // via this method.  If "property" is a valid property understood by this
  // DB implementation, fills "*value" with its current value and returns
  // true.  Otherwise returns false.
  //
  //
  // Valid property names include:
  //
  //  "leveldb.num-files-at-level<N>" - return the number of files at level <N>,
  //     where <N> is an ASCII representation of a level number (e.g. "0").
  //  "leveldb.stats" - returns a multi-line string that describes statistics
  //     about the internal operation of the DB.
  //  "leveldb.sstables" - returns a multi-line string that describes all
  //     of the sstables that make up the db contents.
  virtual bool GetProperty(const Slice& property, std::string* value) = 0;

  // For each i in [0,n-1], store in "sizes[i]", the approximate
  // file system space used by keys in "[range[i].start .. range[i].limit)".
  //
  // Note that the returned sizes measure file system space usage, so
  // if the user data compresses by a factor of ten, the returned
  // sizes will be one-tenth the size of the corresponding user data size.
  //
  // The results may not include the sizes of recently written data.
  virtual void GetApproximateSizes(const Range* range, int n,
                                   uint64_t* sizes) = 0;

  // Compact the underlying storage for the key range [*begin,*end].
  // In particular, deleted and overwritten versions are discarded,
  // and the data is rearranged to reduce the cost of operations
  // needed to access the data.  This operation should typically only
  // be invoked by users who understand the underlying implementation.
  //
  // begin==NULL is treated as a key before all keys in the database.
  // end==NULL is treated as a key after all keys in the database.
  // Therefore the following call will compact the entire database:
  //    db->CompactRange(NULL, NULL);
  virtual void CompactRange(const Slice* begin, const Slice* end) = 0;

  // Suspends the background compaction thread.  This methods
  // returns once suspended.
  virtual void SuspendCompactions() = 0;
  // Resumes a suspended background compation thread.
  virtual void ResumeCompactions() = 0;

#ifdef HAVE_HYPERLEVELDB
  // Create a live backup of a live LevelDB instance.
  // The backup is stored in a directory named "backup-<name>" under the top
  // level of the open LevelDB database.  The implementation is permitted, and
  // even encouraged, to improve the performance of this call through
  // hard-links.
  virtual Status LiveBackup(const Slice& name) = 0;

  // Return an opaque timestamp that identifies the current point in time of the
  // database.  This timestamp may be subsequently presented to the
  // NewReplayIterator method to create a ReplayIterator.
  virtual void GetReplayTimestamp(std::string* timestamp) = 0;

  // Set the lower bound for manual garbage collection.  This method only takes
  // effect when Options.manual_garbage_collection is true.
  virtual void AllowGarbageCollectBeforeTimestamp(const std::string& timestamp) = 0;

  // Validate the timestamp
  virtual bool ValidateTimestamp(const std::string& timestamp) = 0;

  // Compare two timestamps and return -1, 0, 1 for lt, eq, gt
  virtual int CompareTimestamps(const std::string& lhs, const std::string& rhs) = 0;

  // Return a ReplayIterator that returns every write operation performed after
  // the timestamp.
  virtual Status GetReplayIterator(const std::string& timestamp,
                                   ReplayIterator** iter) = 0;

  // Release a previously allocated replay iterator.
  virtual void ReleaseReplayIterator(ReplayIterator* iter) = 0;
#endif
 private:
  // No copying allowed
  DB(const DB&);
  void operator=(const DB&);
};

// Destroy the contents of the specified database.
// Be very careful using this method.
Status DestroyDB(const std::string& name, const Options& options);

// If a DB cannot be opened, you may attempt to call this method to
// resurrect as much of the contents of the database as possible.
// Some data may be lost, so be careful when calling this function
// on a database that contains important information.
Status RepairDB(const std::string& dbname, const Options& options);

};  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_DB_H_
