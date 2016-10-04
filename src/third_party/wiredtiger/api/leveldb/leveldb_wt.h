/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef _INCLUDE_LEVELDB_WT_H
#define _INCLUDE_LEVELDB_WT_H 1

#include "leveldb_wt_config.h"

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/write_batch.h"
#if HAVE_BASHO_LEVELDB
#include "basho/perf_count.h"
#endif

#include "wiredtiger.h"

#define WT_URI          "table:data"
#define WT_CONN_CONFIG                                                  \
        "log=(enabled),checkpoint=(wait=180),checkpoint_sync=false,"    \
        "session_max=8192,mmap=false,"                                  \
        "transaction_sync=(enabled=true,method=none),"
// Note: LSM doesn't split, build full pages from the start
#define WT_TABLE_CONFIG "type=lsm,split_pct=100,leaf_item_max=1KB,"	\
    "lsm=(chunk_size=100MB,bloom_config=(leaf_page_max=8MB)),"
#define	WT_TIMESTAMP_FORMAT "%d.%llu"
// We're also only interested in operations to the user file.  Skip over 
// any changes to the metadata.
// !!! Currently WT guarantees that the metadata file is always at
// fileid 0 and the implementation here only uses one table.  This will
// breakdown if either of those assumptions changes.
#define	WT_VALID_OPERATION(fileid, optype)				\
	((fileid) != 0 &&						\
	 ((optype) == WT_LOGOP_COL_PUT ||				\
	  (optype) == WT_LOGOP_COL_REMOVE ||				\
	  (optype) == WT_LOGOP_ROW_PUT ||				\
	  (optype) == WT_LOGOP_ROW_REMOVE))

using leveldb::Cache;
using leveldb::FilterPolicy;
using leveldb::Iterator;
using leveldb::Options;
using leveldb::ReadOptions;
using leveldb::WriteBatch;
using leveldb::WriteOptions;
using leveldb::Range;
using leveldb::Slice;
using leveldb::Snapshot;
using leveldb::Status;
#if HAVE_BASHOLEVELDB
using leveldb::Value;
#endif
#if HAVE_ROCKSDB
using leveldb::FlushOptions;
using leveldb::ColumnFamilyHandle;
#endif

extern Status WiredTigerErrorToStatus(int wiredTigerError, const char *msg = "");

/* POSIX thread-local storage */
template <class T>
class ThreadLocal {
public:
  static void cleanup(void *val) {
    delete (T *)val;
  }

  ThreadLocal() {
    int ret = pthread_key_create(&key_, cleanup);
    assert(ret == 0);
  }

  ~ThreadLocal() {
    int ret = pthread_key_delete(key_);
    assert(ret == 0);
  }

  T *Get() {
    return (T *)(pthread_getspecific(key_));
  }

  void Set(T *value) {
    int ret = pthread_setspecific(key_, value);
    assert(ret == 0);
  }

private:
  pthread_key_t key_;
};

/* WiredTiger implementations. */
class DbImpl;

/* Context for operations (including snapshots, write batches, transactions) */
class OperationContext {
public:
  OperationContext(WT_CONNECTION *conn) {
    int ret = conn->open_session(conn, NULL, "isolation=snapshot", &session_);
    assert(ret == 0);
    ret = session_->open_cursor(
        session_, WT_URI, NULL, NULL, &cursor_);
    assert(ret == 0);
  }

  ~OperationContext() {
#ifdef WANT_SHUTDOWN_RACES
    int ret = Close();
    assert(ret == 0);
#endif
  }

  int Close() {
    int ret = 0;
    if (session_ != NULL)
      ret = session_->close(session_, NULL);
    session_ = NULL;
    return (ret);
  }

  WT_CURSOR *GetCursor() { return cursor_; }
  void SetCursor(WT_CURSOR *c) { cursor_ = c; }
#ifdef HAVE_ROCKSDB
  WT_CURSOR *GetCursor(u_int i) {
    return (i < cursors_.size()) ? cursors_[i] : NULL;
  }
  void SetCursor(u_int i, WT_CURSOR *c) {
    if (i >= cursors_.size())
      cursors_.resize(i + 1);
    cursors_[i] = c;
  }
#endif
  WT_SESSION *GetSession() { return session_; }

private:
  WT_SESSION *session_;
  WT_CURSOR *cursor_;
#ifdef HAVE_ROCKSDB
  std::vector<WT_CURSOR *> cursors_;
#endif
};

class CacheImpl : public Cache {
public:
  CacheImpl(size_t capacity) : Cache(), capacity_(capacity) {}
  virtual ~CacheImpl() {}

  virtual Handle* Insert(const Slice&, void*, size_t,
      void (*)(const Slice&, void*)) { return 0; }
  virtual Handle* Lookup(const Slice&) { return 0; }
  virtual void Release(Handle*) {}
  virtual void* Value(Handle*) { return 0; }
  virtual void Erase(const Slice&) {}
  virtual uint64_t NewId() { return 0; }

  size_t capacity_;
};

#ifdef HAVE_ROCKSDB
// ColumnFamilyHandleImpl is the class that clients use to access different
// column families. It has non-trivial destructor, which gets called when client
// is done using the column family
class ColumnFamilyHandleImpl : public ColumnFamilyHandle {
 public:
  ColumnFamilyHandleImpl(DbImpl* db, std::string const &name, uint32_t id) : db_(db), id_(id), name_(name) {}
  ColumnFamilyHandleImpl(const ColumnFamilyHandleImpl &copyfrom) : db_(copyfrom.db_), id_(copyfrom.id_), name_(copyfrom.name_) {}
  virtual ~ColumnFamilyHandleImpl() {}
  virtual uint32_t GetID() const { return id_; }

  std::string const &GetName() const { return name_; }
  std::string const GetURI() const { return "table:" + name_; }

 private:
  DbImpl* db_;
  uint32_t id_;
  std::string const name_;
};
#endif

class IteratorImpl : public Iterator {
public:
  IteratorImpl(DbImpl *db, WT_CURSOR *cursor, uint32_t id=0) : db_(db), cursor_(cursor), id_(id) {}
  virtual ~IteratorImpl();

  // An iterator is either positioned at a key/value pair, or
  // not valid.  This method returns true iff the iterator is valid.
  virtual bool Valid() const { return valid_; }

  virtual void SeekToFirst();

  virtual void SeekToLast();

  virtual void Seek(const Slice& target);

  virtual void Next();

  virtual void Prev();

  virtual Slice key() const {
    return key_;
  }

  virtual Slice value() const {
    return value_;
  }

  virtual Status status() const {
    return status_;
  }

private:
  DbImpl *db_;
  WT_CURSOR *cursor_;
  Slice key_, value_;
  Status status_;
  bool valid_;
  uint32_t id_;

  void SetError(int wiredTigerError) {
    valid_ = false;
    status_ = WiredTigerErrorToStatus(wiredTigerError, NULL);
  }

  // No copying allowed
  IteratorImpl(const IteratorImpl&);
  void operator=(const IteratorImpl&);
};

class SnapshotImpl : public Snapshot {
friend class DbImpl;
friend class IteratorImpl;
public:
  SnapshotImpl(DbImpl *db);
  virtual ~SnapshotImpl() { delete context_; }
protected:
  OperationContext *GetContext() const { return context_; }
  Status GetStatus() const { return status_; }
  Status SetupTransaction();
private:
  DbImpl *db_;
  OperationContext *context_;
  Status status_;
};

class DbImpl : public leveldb::DB {
friend class IteratorImpl;
friend class SnapshotImpl;
public:
  DbImpl(WT_CONNECTION *conn) :
    DB(), conn_(conn), context_(new ThreadLocal<OperationContext>) {}
  virtual ~DbImpl() {
    delete context_;
    int ret = conn_->close(conn_, NULL);
    assert(ret == 0);
  }

  virtual Status Put(const WriteOptions& options,
         const Slice& key,
         const Slice& value);

  virtual Status Delete(const WriteOptions& options, const Slice& key);

  virtual Status Write(const WriteOptions& options, WriteBatch* updates);

  virtual Status Get(const ReadOptions& options,
         const Slice& key, std::string* value);

#if HAVE_BASHOLEVELDB
  virtual Status Get(const ReadOptions& options,
         const Slice& key, Value* value);
#endif

#ifdef HAVE_HYPERLEVELDB
  virtual Status LiveBackup(const Slice& name);
  virtual void GetReplayTimestamp(std::string* timestamp);
  virtual void AllowGarbageCollectBeforeTimestamp(const std::string& timestamp);
  virtual bool ValidateTimestamp(const std::string& timestamp);
  virtual int CompareTimestamps(const std::string& lhs, const std::string& rhs);
  virtual Status GetReplayIterator(const std::string& timestamp,
             leveldb::ReplayIterator** iter);
  virtual void ReleaseReplayIterator(leveldb::ReplayIterator* iter);
#endif

#ifdef HAVE_ROCKSDB
  virtual Status CreateColumnFamily(const Options& options,
                                    const std::string& column_family_name,
                                    ColumnFamilyHandle** handle);

  // Drop a column family specified by column_family handle. This call
  // only records a drop record in the manifest and prevents the column
  // family from flushing and compacting.
  virtual Status DropColumnFamily(ColumnFamilyHandle* column_family);

  // Set the database entry for "key" to "value".
  // Returns OK on success, and a non-OK status on error.
  // Note: consider setting options.sync = true.
  virtual Status Put(const WriteOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value);

  // Remove the database entry (if any) for "key".  Returns OK on
  // success, and a non-OK status on error.  It is not an error if "key"
  // did not exist in the database.
  // Note: consider setting options.sync = true.
  virtual Status Delete(const WriteOptions& options,
                        ColumnFamilyHandle* column_family,
                        const Slice& key);

  // Merge the database entry for "key" with "value".  Returns OK on success,
  // and a non-OK status on error. The semantics of this operation is
  // determined by the user provided merge_operator when opening DB.
  // Note: consider setting options.sync = true.
  virtual Status Merge(const WriteOptions& options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value);

  // May return some other Status on an error.
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     std::string* value);

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
      const std::vector<Slice>& keys, std::vector<std::string>* values);

  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family);

  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value);

  // Flush all mem-table data.
  virtual Status Flush(const FlushOptions& options,
                       ColumnFamilyHandle* column_family);

  ColumnFamilyHandleImpl *GetCF(uint32_t id) {
    return (id < columns_.size()) ? static_cast<ColumnFamilyHandleImpl *>(columns_[id]) : NULL;
  }
  void SetColumns(std::vector<ColumnFamilyHandle *> &cols) {
    columns_ = cols;
  }
#endif

  virtual Iterator* NewIterator(const ReadOptions& options);

  virtual const Snapshot* GetSnapshot();

  virtual void ReleaseSnapshot(const Snapshot* snapshot);

  virtual bool GetProperty(const Slice& property, std::string* value);

  virtual void GetApproximateSizes(const Range* range, int n,
           uint64_t* sizes);

  virtual void CompactRange(const Slice* begin, const Slice* end);

  virtual void SuspendCompactions();
  
  virtual void ResumeCompactions();

  OperationContext *GetContext() {
    OperationContext *ctx = context_->Get();
    if (ctx == NULL) {
      ctx = NewContext();
      context_->Set(ctx);
    }
    return (ctx);
  }

private:
  WT_CONNECTION *conn_;
  ThreadLocal<OperationContext> *context_;
#ifdef HAVE_ROCKSDB
  std::vector<ColumnFamilyHandle*> columns_;
#endif

  OperationContext *NewContext() {
    return new OperationContext(conn_);
  }

  OperationContext *GetContext(const ReadOptions &options) {
    if (options.snapshot == NULL)
      return GetContext();
    else {
      const SnapshotImpl *si =
          static_cast<const SnapshotImpl *>(options.snapshot);
      assert(si->GetStatus().ok());
      return si->GetContext();
    }
  }

  // No copying allowed
  DbImpl(const DbImpl&);
  void operator=(const DbImpl&);
};

// Implemention of WriteBatch::Handler
class WriteBatchHandler : public WriteBatch::Handler {
public:
  WriteBatchHandler(DbImpl *db, OperationContext *context) : db_(db), context_(context), status_(0) {}
  virtual ~WriteBatchHandler() {}
  int GetWiredTigerStatus() { return status_; }

  virtual void Put(const Slice& key, const Slice& value); 

  virtual void Delete(const Slice& key);

#ifdef HAVE_ROCKSDB
  // Implementations are in rocksdb_wt.cc
  virtual Status PutCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value);
  virtual Status DeleteCF(uint32_t column_family_id, const Slice& key);
#endif

private:
  DbImpl *db_;
  OperationContext *context_;
  int status_;
};

#endif
