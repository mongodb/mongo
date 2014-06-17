/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *   All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include "leveldb_wt.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sstream>

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
#ifdef HAVE_HYPERLEVELDB
namespace leveldb {
class ReplayIterator;
}
#endif

#define WT_URI          "table:data"
#define WT_CONN_CONFIG  "log=(enabled),checkpoint_sync=false," \
                         "transaction_sync=none,session_max=256,"
#define WT_TABLE_CONFIG  "type=lsm,leaf_page_max=4KB,leaf_item_max=1KB,"

/* Destructors required for interfaces. */
leveldb::DB::~DB() {}
Snapshot::~Snapshot() {}

Status WiredTigerErrorToStatus(int wiredTigerError, const char *msg) {
  if (wiredTigerError == 0)
    return Status::OK();

  if (msg == NULL)
    msg = wiredtiger_strerror(wiredTigerError);

  if (wiredTigerError == WT_NOTFOUND)
    return Status::NotFound(Slice(msg));
  else if (wiredTigerError == WT_ERROR || wiredTigerError == WT_PANIC)
    return Status::Corruption(Slice(msg));
  else if (wiredTigerError == WT_DEADLOCK)
    return Status::IOError("DEADLOCK"); // TODO: Is this the best translation?
  else if (wiredTigerError == ENOTSUP)
    return Status::NotSupported(Slice(msg));
  else if (wiredTigerError == EINVAL)
    return Status::InvalidArgument(Slice(msg));
  else if (wiredTigerError == EPERM || wiredTigerError == ENOENT ||
      wiredTigerError == EIO || wiredTigerError == EBADF ||
      wiredTigerError == EEXIST || wiredTigerError == ENOSPC)
    return Status::IOError(Slice(msg));
  else
    return Status::Corruption(Slice(msg));
}

/* Iterators, from leveldb/table/iterator.cc */
Iterator::Iterator() {
  cleanup_.function = NULL;
  cleanup_.next = NULL;
}

Iterator::~Iterator() {
  if (cleanup_.function != NULL) {
    (*cleanup_.function)(cleanup_.arg1, cleanup_.arg2);
    for (Cleanup* c = cleanup_.next; c != NULL; ) {
      (*c->function)(c->arg1, c->arg2);
      Cleanup* next = c->next;
      delete c;
      c = next;
    }
  }
}

void Iterator::RegisterCleanup(CleanupFunction func, void* arg1, void* arg2) {
  assert(func != NULL);
  Cleanup* c;
  if (cleanup_.function == NULL) {
    c = &cleanup_;
  } else {
    c = new Cleanup;
    c->next = cleanup_.next;
    cleanup_.next = c;
  }
  c->function = func;
  c->arg1 = arg1;
  c->arg2 = arg2;
}

namespace {
class EmptyIterator : public Iterator {
 public:
  EmptyIterator(const Status& s) : status_(s) { }
  virtual bool Valid() const { return false; }
  virtual void Seek(const Slice& target) { }
  virtual void SeekToFirst() { }
  virtual void SeekToLast() { }
  virtual void Next() { assert(false); }
  virtual void Prev() { assert(false); }
  Slice key() const { assert(false); return Slice(); }
  Slice value() const { assert(false); return Slice(); }
  virtual Status status() const { return status_; }
 private:
  Status status_;
};
}  // namespace

Iterator* NewEmptyIterator() {
  return new EmptyIterator(Status::OK());
}

Iterator* NewErrorIterator(const Status& status) {
  return new EmptyIterator(status);
}

namespace {
class FilterPolicyImpl : public FilterPolicy {
public:
  FilterPolicyImpl(int bits_per_key) : bits_per_key_(bits_per_key) {}
  ~FilterPolicyImpl() {}
  virtual const char *Name() const { return "FilterPolicyImpl"; }
  virtual void CreateFilter(const Slice *keys, int n, std::string *dst) const {}
  virtual bool KeyMayMatch(const Slice &key, const Slice &filter) const {}

  int bits_per_key_;
};
};


namespace leveldb {
FilterPolicy::~FilterPolicy() {}

const FilterPolicy *NewBloomFilterPolicy(int bits_per_key) {
  return new FilterPolicyImpl(bits_per_key);
}

Cache::~Cache() {}

class CacheImpl : public Cache {
public:
  CacheImpl(size_t capacity) : Cache(), capacity_(capacity) {}

  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
      void (*deleter)(const Slice& key, void* value)) { return 0; }
  virtual Handle* Lookup(const Slice& key) { return 0; }
  virtual void Release(Handle* handle) {}
  virtual void* Value(Handle* handle) { return 0; }
  virtual void Erase(const Slice& key) {}
  virtual uint64_t NewId() { return 0; }

  size_t capacity_;
};

Cache *NewLRUCache(size_t capacity) {
  return new CacheImpl(capacity);
}

Status DestroyDB(const std::string& name, const Options& options) {
  WT_CONNECTION *conn;
  int ret, t_ret;
  /* If the database doesn't exist, there is nothing to destroy. */
  if (access((name + "/WiredTiger").c_str(), F_OK) != 0)
    return Status::OK();
  if ((ret = ::wiredtiger_open(name.c_str(), NULL, NULL, &conn)) != 0)
    return WiredTigerErrorToStatus(ret, NULL);
  WT_SESSION *session;
  if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
    goto cleanup;
  if ((ret = session->drop(session, WT_URI, "force")) != 0)
    goto cleanup;

cleanup:
  if ((t_ret = conn->close(conn, NULL)) != 0 && ret == 0)
    ret = t_ret;
  return WiredTigerErrorToStatus(ret, NULL);
}

Status RepairDB(const std::string& dbname, const Options& options) {
  return Status::NotSupported("sorry!");
}
}

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

  T *get() {
    return (T *)(pthread_getspecific(key_));
  }

  void set(T *value) {
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
  OperationContext(WT_CONNECTION *conn) : conn_(conn), in_use_(false) {
    int ret = conn->open_session(conn, NULL, "isolation=snapshot", &session_);
    assert(ret == 0);
    ret = session_->open_cursor(
        session_, WT_URI, NULL, NULL, &cursor_);
    assert(ret == 0);
  }

  ~OperationContext() {
#ifdef WANT_SHUTDOWN_RACES
    int ret = session_->close(session_, NULL);
    assert(ret == 0);
#endif
  }

  WT_CURSOR *getCursor();
  void releaseCursor(WT_CURSOR *cursor);

private:
  WT_CONNECTION *conn_;
  WT_SESSION *session_;
  WT_CURSOR *cursor_;
  bool in_use_;
};

class IteratorImpl : public Iterator {
public:
  IteratorImpl(DbImpl *db, const ReadOptions &options);
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
  bool snapshot_iterator_;

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
  SnapshotImpl(DbImpl *db) :
    Snapshot(), db_(db), cursor_(NULL), status_(Status::OK()) {}
  virtual ~SnapshotImpl() {}
protected:
  WT_CURSOR *getCursor() const { return cursor_; }
  Status getStatus() const { return status_; }
  Status setupTransaction();
  Status releaseTransaction();
private:
  DbImpl *db_;
  WT_CURSOR *cursor_;
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

#ifdef HAVE_HYPERLEVELDB
  virtual Status LiveBackup(const Slice& name) {
    return Status::NotSupported("sorry!");
  }
  virtual void GetReplayTimestamp(std::string* timestamp) {}
  virtual void AllowGarbageCollectBeforeTimestamp(const std::string& timestamp) {}
  virtual bool ValidateTimestamp(const std::string& timestamp) {}
  virtual int CompareTimestamps(const std::string& lhs, const std::string& rhs) {}
  virtual Status GetReplayIterator(const std::string& timestamp,
             leveldb::ReplayIterator** iter) { return Status::NotSupported("sorry!"); }
  virtual void ReleaseReplayIterator(leveldb::ReplayIterator* iter) {}
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

private:
  WT_CONNECTION *conn_;
  ThreadLocal<OperationContext> *context_;

  OperationContext *getContext() {
    OperationContext *ctx = context_->get();
    if (ctx == NULL) {
      ctx = new OperationContext(conn_);
      context_->set(ctx);
    }
    return (ctx);
  }

  // No copying allowed
  DbImpl(const DbImpl&);
  void operator=(const DbImpl&);

protected:
  WT_CURSOR *getCursor() { return getContext()->getCursor(); }
  void releaseCursor(WT_CURSOR *cursor) { getContext()->releaseCursor(cursor); }
};

// Return a cursor for the current operation to use. In the "normal" case
// we will return the cursor opened when the OperationContext was created.
// If the thread this OperationContext belongs to requires more than one
// cursor (for example they start a read snapshot while doing updates), we
// open a new session/cursor for each parallel operation.
WT_CURSOR *OperationContext::getCursor()
{
  int ret;
  if (!in_use_) {
    in_use_ = true;
    return cursor_;
  } else {
    WT_SESSION *session;
    WT_CURSOR *cursor;
    if ((ret = conn_->open_session(
            conn_, NULL, "isolation=snapshot", &session)) != 0)
      return NULL;
    if ((ret = session->open_cursor(
        session, WT_URI, NULL, NULL, &cursor)) != 0)
      return NULL;
    return cursor;
  }
}

void OperationContext::releaseCursor(WT_CURSOR *cursor)
{
  if (cursor == cursor_)
    in_use_ = false;
  else {
    WT_SESSION *session = cursor->session;
    int ret = session->close(session, NULL);
    assert(ret == 0);
  }
}

Status
leveldb::DB::Open(const Options &options, const std::string &name, leveldb::DB **dbptr)
{
  // Build the wiredtiger_open config.
  std::stringstream s_conn;
  s_conn << WT_CONN_CONFIG;
  if (options.create_if_missing) {
    (void)mkdir(name.c_str(), 0777);
    s_conn << "create,";
  }
  if (options.error_if_exists)
    s_conn << "exclusive,";
  if (options.compression == kSnappyCompression)
    s_conn << "extensions=[libwiredtiger_snappy.so],";
  size_t cache_size = 25 * options.write_buffer_size;
  if (options.block_cache)
    cache_size += ((CacheImpl *)options.block_cache)->capacity_;
  s_conn << "cache_size=" << cache_size << ",";
  std::string conn_config = s_conn.str();

  WT_CONNECTION *conn;
  int ret = ::wiredtiger_open(name.c_str(), NULL, conn_config.c_str(), &conn);
  if (ret == ENOENT)
    return Status::NotFound(Slice("Database does not exist."));
  else if (ret == EEXIST)
    return Status::NotFound(Slice("Database already exists."));
  else if (ret != 0)
    return WiredTigerErrorToStatus(ret, NULL);

  if (options.create_if_missing) {
    std::stringstream s_table;
    s_table << WT_TABLE_CONFIG;
    s_table << "internal_page_max=" << options.block_size << ",";
    s_table << "leaf_page_max=" << options.block_size << ",";
    if (options.compression == kSnappyCompression)
      s_table << "block_compressor=snappy,";
    s_table << "lsm=(";
    s_table << "chunk_size=" << options.write_buffer_size << ",";
    if (options.filter_policy) {
      int bits = ((FilterPolicyImpl *)options.filter_policy)->bits_per_key_;
      s_table << "bloom_bit_count=" << bits << ",";
      // Approximate the optimal number of hashes
      s_table << "bloom_hash_count=" << (int)(0.6 * bits) << ",";
    }
    s_table << "),";
    WT_SESSION *session;
    std::string table_config = s_table.str();
    if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
      goto err;
    if ((ret = session->create(session, WT_URI, table_config.c_str())) != 0)
      goto err;
    if ((ret = session->close(session, NULL)) != 0)
      goto err;
  }

  if (ret != 0) {
err:
    conn->close(conn, NULL);
    return WiredTigerErrorToStatus(ret, NULL);
  }
  *dbptr = new DbImpl(conn);
  return Status::OK();
}

// Set the database entry for "key" to "value".  Returns OK on success,
// and a non-OK status on error.
// Note: consider setting options.sync = true.
Status
DbImpl::Put(const WriteOptions& options,
       const Slice& key, const Slice& value)
{
  WT_CURSOR *cursor = getCursor();
  WT_ITEM item;

  item.data = key.data();
  item.size = key.size();
  cursor->set_key(cursor, &item);
  item.data = value.data();
  item.size = value.size();
  cursor->set_value(cursor, &item);
  int ret = cursor->insert(cursor);
  releaseCursor(cursor);
  return WiredTigerErrorToStatus(ret, NULL);
}

// Remove the database entry (if any) for "key".  Returns OK on
// success, and a non-OK status on error.  It is not an error if "key"
// did not exist in the database.
// Note: consider setting options.sync = true.
Status
DbImpl::Delete(const WriteOptions& options, const Slice& key)
{
  WT_CURSOR *cursor = getCursor();
  WT_ITEM item;

  item.data = key.data();
  item.size = key.size();
  cursor->set_key(cursor, &item);
  int ret = cursor->remove(cursor);
  // Reset the WiredTiger cursor so it doesn't keep any pages pinned. Track
  // failures in debug builds since we don't expect failure, but don't pass
  // failures on - it's not necessary for correct operation.
  int t_ret = cursor->reset(cursor);
  assert(t_ret == 0);
  releaseCursor(cursor);
  return WiredTigerErrorToStatus(ret, NULL);
}

// Implement WriteBatch::Handler
class WriteBatchHandler : public WriteBatch::Handler {
public:
  WriteBatchHandler(WT_CURSOR *cursor) : cursor_(cursor), status_(0) {}
  virtual ~WriteBatchHandler() {}
  int getWiredTigerStatus() { return status_; }

  virtual void Put(const Slice& key, const Slice& value) {
    WT_ITEM item;

    item.data = key.data();
    item.size = key.size();
    cursor_->set_key(cursor_, &item);
    item.data = value.data();
    item.size = value.size();
    cursor_->set_value(cursor_, &item);
    int ret = cursor_->insert(cursor_);
    if (ret != 0 && status_ == 0)
      status_ = ret;
  }

  virtual void Delete(const Slice& key) {
    WT_ITEM item;

    item.data = key.data();
    item.size = key.size();
    cursor_->set_key(cursor_, &item);
    int ret = cursor_->remove(cursor_);
    if (ret != 0 && status_ == 0)
      status_ = ret;
  }

private:
  WT_CURSOR *cursor_;
  int status_;
};

// Apply the specified updates to the database.
// Returns OK on success, non-OK on failure.
// Note: consider setting options.sync = true.
Status
DbImpl::Write(const WriteOptions& options, WriteBatch* updates)
{
  Status status = Status::OK();
  WT_CURSOR *cursor = getCursor();
  WT_SESSION *session = cursor->session;
  const char *errmsg = NULL;
  int ret, t_ret;

  for (;;) {
    if ((ret = session->begin_transaction(session, NULL)) != 0) {
      errmsg = "Begin transaction failed in Write batch";
      goto err;
    }

    WriteBatchHandler handler(cursor);
    status = updates->Iterate(&handler);
    if ((ret = handler.getWiredTigerStatus()) != WT_DEADLOCK)
      break;
    // Roll back the transaction on deadlock so we can try again
    if ((ret = session->rollback_transaction(session, NULL)) != 0) {
      errmsg = "Rollback transaction failed in Write batch";
      goto err;
    }
  }

  if (status.ok() && ret == 0)
    ret = session->commit_transaction(session, NULL);
  else if (ret == 0)
    ret = session->rollback_transaction(session, NULL);

err:
  releaseCursor(cursor);
  if (status.ok() && ret != 0)
    status = WiredTigerErrorToStatus(ret, NULL);
  return status;
}

// If the database contains an entry for "key" store the
// corresponding value in *value and return OK.
//
// If there is no entry for "key" leave *value unchanged and return
// a status for which Status::IsNotFound() returns true.
//
// May return some other Status on an error.
Status
DbImpl::Get(const ReadOptions& options,
       const Slice& key, std::string* value)
{
  WT_CURSOR *cursor;
  WT_ITEM item;
  const SnapshotImpl *si = NULL;
  const char *errmsg = NULL;

  // Read options can contain a snapshot for us to use
  if (options.snapshot == NULL) {
    cursor = getCursor();
  } else {
    si = static_cast<const SnapshotImpl *>(options.snapshot);
    if (!si->getStatus().ok())
      return si->getStatus();
    cursor = si->getCursor();
  }

  item.data = key.data();
  item.size = key.size();
  cursor->set_key(cursor, &item);
  int ret = cursor->search(cursor);
  if (ret == 0) {
    ret = cursor->get_value(cursor, &item);
    if (ret == 0)
      *value = std::string((const char *)item.data, item.size);
  } else if (ret == WT_NOTFOUND)
    errmsg = "DB::Get key not found";
err:
  // There is no need to release the cursor if we are in a snapshot
  if (si != NULL)
    releaseCursor(cursor);
  return WiredTigerErrorToStatus(ret, errmsg);
}

// Return a heap-allocated iterator over the contents of the database.
// The result of NewIterator() is initially invalid (caller must
// call one of the Seek methods on the iterator before using it).
//
// Caller should delete the iterator when it is no longer needed.
// The returned iterator should be deleted before this db is deleted.
Iterator *
DbImpl::NewIterator(const ReadOptions& options)
{
  return new IteratorImpl(this, options);
}

// Return a handle to the current DB state.  Iterators created with
// this handle will all observe a stable snapshot of the current DB
// state.  The caller must call ReleaseSnapshot(result) when the
// snapshot is no longer needed.
const Snapshot *
DbImpl::GetSnapshot()
{
  SnapshotImpl *snapshot = new SnapshotImpl(this);
  Status status = snapshot->setupTransaction();
  if (!status.ok()) {
    delete snapshot;
    // TODO: Flag an error here?
    return NULL;
  }
  return snapshot;
}

// Release a previously acquired snapshot.  The caller must not
// use "snapshot" after this call.
void
DbImpl::ReleaseSnapshot(const Snapshot* snapshot)
{
  SnapshotImpl *si =
    static_cast<SnapshotImpl *>(const_cast<Snapshot *>(snapshot));
  if (si != NULL) {
    si->releaseTransaction();
    delete si;
  }
}

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
bool
DbImpl::GetProperty(const Slice& property, std::string* value)
{
  /* Not supported */
  return false;
}

// For each i in [0,n-1], store in "sizes[i]", the approximate
// file system space used by keys in "[range[i].start .. range[i].limit)".
//
// Note that the returned sizes measure file system space usage, so
// if the user data compresses by a factor of ten, the returned
// sizes will be one-tenth the size of the corresponding user data size.
//
// The results may not include the sizes of recently written data.
void
DbImpl::GetApproximateSizes(const Range* range, int n,
         uint64_t* sizes)
{
  int i;

  /* XXX Not supported */
  for (i = 0; i < n; i++)
    sizes[i] = 1;
}

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
void
DbImpl::CompactRange(const Slice* begin, const Slice* end)
{
  // The compact doesn't need a cursor, but the context always opens a
  // cursor when opening the session - so grab that, and use the session.
  WT_CURSOR *cursor = getCursor();
  WT_SESSION *session = cursor->session;
  int ret = session->compact(session, WT_URI, NULL);
  assert(ret == 0);
  releaseCursor(cursor);
}

// Suspends the background compaction thread.  This methods
// returns once suspended.
void
DbImpl::SuspendCompactions()
{
  /* Not supported */
}

// Resumes a suspended background compation thread.
void
DbImpl::ResumeCompactions()
{
  /* Not supported */
}

IteratorImpl::IteratorImpl(DbImpl *db, const ReadOptions &options) :
    cursor_(NULL), db_(db), status_(Status::OK()), valid_(false)
{
  if (options.snapshot == NULL) {
    cursor_ = db_->getCursor();
    snapshot_iterator_ = false;
  } else {
    const SnapshotImpl *si =
      static_cast<const SnapshotImpl *>(options.snapshot);
    cursor_ = si->getCursor();
    snapshot_iterator_ = true;
  }
}

IteratorImpl::~IteratorImpl()
{
  if (!snapshot_iterator_)
    db_->releaseCursor(cursor_);
}

// Position at the first key in the source.  The iterator is Valid()
// after this call iff the source is not empty.
void
IteratorImpl::SeekToFirst()
{
  int ret;
  WT_ITEM item;

  if (!Status().ok())
    return;

  if ((ret = cursor_->reset(cursor_)) != 0) {
    SetError(ret);
    return;
  }
  ret = cursor_->next(cursor_);
  if (ret == WT_NOTFOUND) {
    valid_ = false;
    return;
  } else if (ret != 0) {
    SetError(ret);
    return;
  }
  if ((ret = cursor_->get_key(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  key_ = Slice((const char *)item.data, item.size);
  if ((ret = cursor_->get_value(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  value_ = Slice((const char *)item.data, item.size);
  valid_ = true;
}

// Position at the last key in the source.  The iterator is
// Valid() after this call iff the source is not empty.
void
IteratorImpl::SeekToLast()
{
  int ret;
  WT_ITEM item;

  if (!Status().ok())
    return;

  if ((ret = cursor_->reset(cursor_)) != 0) {
    SetError(ret);
    return;
  }
  ret = cursor_->prev(cursor_);
  if (ret == WT_NOTFOUND) {
    valid_ = false;
    return;
  } else if (ret != 0) {
    SetError(ret);
    return;
  }
  if ((ret = cursor_->get_key(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  key_ = Slice((const char *)item.data, item.size);
  if ((ret = cursor_->get_value(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  value_ = Slice((const char *)item.data, item.size);
  valid_ = true;
}

// Position at the first key in the source that at or past target
// The iterator is Valid() after this call iff the source contains
// an entry that comes at or past target.
void
IteratorImpl::Seek(const Slice& target)
{
  WT_ITEM item;

  if (!Status().ok())
    return;

  item.data = target.data();
  item.size = target.size();
  cursor_->set_key(cursor_, &item);
  int cmp, ret = cursor_->search_near(cursor_, &cmp);
  if (ret == 0 && cmp < 0)
    ret = cursor_->next(cursor_);
  if (ret != 0) {
    if (ret != WT_NOTFOUND)
      SetError(ret);
    valid_ = false;
    return;
  }
  if ((ret = cursor_->get_key(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  key_ = Slice((const char *)item.data, item.size);
  if ((ret = cursor_->get_value(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  value_ = Slice((const char *)item.data, item.size);
  valid_ = true;
}

// Moves to the next entry in the source.  After this call, Valid() is
// true iff the iterator was not positioned at the last entry in the source.
// REQUIRES: Valid()
void
IteratorImpl::Next()
{
  int ret;
  WT_ITEM item;

  if (!Status().ok())
    return;

  if (!valid_) {
    SetError(EINVAL);
    return;
  }

  ret = cursor_->next(cursor_);
  if (ret != 0) {
    if (ret != WT_NOTFOUND)
      SetError(ret);
    valid_ = false;
    return;
  }
  if ((ret = cursor_->get_key(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  key_ = Slice((const char *)item.data, item.size);
  if ((ret = cursor_->get_value(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  value_ = Slice((const char *)item.data, item.size);
  valid_ = true;
}

// Moves to the previous entry in the source.  After this call, Valid() is
// true iff the iterator was not positioned at the first entry in source.
// REQUIRES: Valid()
void
IteratorImpl::Prev()
{
  WT_ITEM item;

  if (!Status().ok())
    return;

  if (!valid_) {
    SetError(EINVAL);
    return;
  }

  int ret = cursor_->prev(cursor_);
  if (ret != 0) {
    if (ret != WT_NOTFOUND)
      SetError(ret);
    valid_ = false;
    return;
  }
  if ((ret = cursor_->get_key(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  key_ = Slice((const char *)item.data, item.size);
  if ((ret = cursor_->get_value(cursor_, &item)) != 0) {
    SetError(ret);
    return;
  }
  value_ = Slice((const char *)item.data, item.size);
  valid_ = true;
}

// Implementation for WiredTiger specific read snapshot
Status SnapshotImpl::setupTransaction()
{
  cursor_ = db_->getCursor();
  WT_SESSION *session = cursor_->session;
  int ret = session->begin_transaction(session, NULL);
  return WiredTigerErrorToStatus(ret, NULL);
}

Status SnapshotImpl::releaseTransaction()
{
  WT_SESSION *session = cursor_->session;
  int ret = session->commit_transaction(session, NULL);
  db_->releaseCursor(cursor_);

  return WiredTigerErrorToStatus(ret, NULL);
}
