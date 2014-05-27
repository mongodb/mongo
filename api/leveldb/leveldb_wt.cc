/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include "leveldb_wt.h"

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
#else
using leveldb::Value;
#endif

#define	WT_URI	"table:data"
#define	WT_CONFIG	"type=lsm,leaf_page_max=4KB,leaf_item_max=1KB"

/* Destructors required for interfaces. */
leveldb::DB::~DB() {}
Snapshot::~Snapshot() {}

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

namespace leveldb {
const FilterPolicy *NewBloomFilterPolicy(int bits_per_key) {
  return 0;
}

Cache *NewLRUCache(size_t capacity) {
  return 0;
}

Status DestroyDB(const std::string& name, const Options& options) {
  fprintf(stderr, "DestroyDB %s", name.c_str());
  return Status::NotSupported("sorry!");
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
	OperationContext(WT_CONNECTION *conn) {
		int ret = conn->open_session(conn, NULL, NULL, &session_);
		assert(ret == 0);
		ret = session_->open_cursor(
		    session_, WT_URI, NULL, NULL, &cursor_);
		assert(ret == 0);
	}

	~OperationContext() {
		int ret = session_->close(session_, NULL);
		assert(ret == 0);
	}

	WT_CURSOR *getCursor() { return cursor_; }

private:
	WT_SESSION *session_;
	WT_CURSOR *cursor_;
};

class IteratorImpl : public Iterator {
public:
	IteratorImpl(WT_CURSOR *cursor, DbImpl *db, const ReadOptions &options) : cursor_(cursor), valid_(false) {}
	virtual ~IteratorImpl() {}

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
	WT_CURSOR *cursor_;
	Slice key_, value_;
	Status status_;
	bool valid_;

	// No copying allowed
	IteratorImpl(const IteratorImpl&);
	void operator=(const IteratorImpl&);
};

class SnapshotImpl : public Snapshot {
public:
	SnapshotImpl(DbImpl *db) : Snapshot() {}
	virtual ~SnapshotImpl() {}
};

class DbImpl : public leveldb::DB {
public:
	DbImpl(WT_CONNECTION *conn) : DB(), conn_(conn), context_(new ThreadLocal<OperationContext>) {}
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
	virtual Status LiveBackup(const Slice& name) { return Status::NotSupported("sorry!"); }
	virtual void GetReplayTimestamp(std::string* timestamp) {}
	virtual void AllowGarbageCollectBeforeTimestamp(const std::string& timestamp) {}
	virtual bool ValidateTimestamp(const std::string& timestamp) {}
	virtual int CompareTimestamps(const std::string& lhs, const std::string& rhs) {}
	virtual Status GetReplayIterator(const std::string& timestamp,
					   leveldb::ReplayIterator** iter) { return Status::NotSupported("sorry!"); }
	virtual void ReleaseReplayIterator(leveldb::ReplayIterator* iter) {}
#else
	virtual Status Get(const ReadOptions& options,
		     const Slice& key, Value* value);
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

	WT_CURSOR *getCursor() { return getContext()->getCursor(); }

	// No copying allowed
	DbImpl(const DbImpl&);
	void operator=(const DbImpl&);
};

Status
leveldb::DB::Open(const Options &options, const std::string &name, leveldb::DB **dbptr)
{
	WT_CONNECTION *conn;
	int ret = ::wiredtiger_open(name.c_str(), NULL, "create", &conn);
	assert(ret == 0);

	WT_SESSION *session;
	ret = conn->open_session(conn, NULL, NULL, &session);
	assert(ret == 0);
	ret = session->create(session, WT_URI, WT_CONFIG);
	assert(ret == 0);
	ret = session->close(session, NULL);
	assert(ret == 0);

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
	assert(ret == 0);
	return Status::OK();
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
	assert(ret == 0);
	return Status::OK();
}

// Implement WriteBatch::Handler
class WriteBatchHandler : public WriteBatch::Handler {
public:
	WriteBatchHandler(WT_CURSOR *cursor) : cursor_(cursor), status_(0) {}
	virtual ~WriteBatchHandler() {}
	int getStatus() { return status_; }

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
	WT_CURSOR *cursor = getCursor();
	WriteBatchHandler handler(cursor);
	Status status = updates->Iterate(&handler);
	assert(handler.getStatus() == 0);
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
	WT_CURSOR *cursor = getCursor();
	WT_ITEM item;

	item.data = key.data();
	item.size = key.size();
	cursor->set_key(cursor, &item);
	int ret = cursor->search(cursor);
	if (ret == WT_NOTFOUND)
		return Status::NotFound("DB::Get key not found");
	assert(ret == 0);
	ret = cursor->get_value(cursor, &item);
	assert(ret == 0);
	*value = std::string((const char *)item.data, item.size);
	return Status::OK();
}

#ifndef HAVE_HYPERLEVELDB
Status
DbImpl::Get(const ReadOptions& options,
	     const Slice& key, Value* value)
{
	WT_CURSOR *cursor = getCursor();
	WT_ITEM item;

	item.data = key.data();
	item.size = key.size();
	cursor->set_key(cursor, &item);
	int ret = cursor->search(cursor);
	if (ret == WT_NOTFOUND)
		return Status::NotFound("DB::Get key not found");
	assert(ret == 0);
	ret = cursor->get_value(cursor, &item);
	assert(ret == 0);
	value->assign((const char *)item.data, item.size);
	return Status::OK();
}
#endif

// Return a heap-allocated iterator over the contents of the database.
// The result of NewIterator() is initially invalid (caller must
// call one of the Seek methods on the iterator before using it).
//
// Caller should delete the iterator when it is no longer needed.
// The returned iterator should be deleted before this db is deleted.
Iterator *
DbImpl::NewIterator(const ReadOptions& options)
{
	return new IteratorImpl(getCursor(), this, options);
}

// Return a handle to the current DB state.  Iterators created with
// this handle will all observe a stable snapshot of the current DB
// state.  The caller must call ReleaseSnapshot(result) when the
// snapshot is no longer needed.
const Snapshot *
DbImpl::GetSnapshot()
{
	return new SnapshotImpl(this);
}

// Release a previously acquired snapshot.  The caller must not
// use "snapshot" after this call.
void
DbImpl::ReleaseSnapshot(const Snapshot* snapshot)
{
	delete (SnapshotImpl *)snapshot;
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
	/* Not supported */
	assert(0);
}

// Suspends the background compaction thread.  This methods
// returns once suspended.
void
DbImpl::SuspendCompactions()
{
	/* Not supported */
	assert(0);
}

// Resumes a suspended background compation thread.
void
DbImpl::ResumeCompactions()
{
	/* Not supported */
	assert(0);
}

// Position at the first key in the source.  The iterator is Valid()
// after this call iff the source is not empty.
void
IteratorImpl::SeekToFirst()
{
	WT_ITEM item;

	int ret = cursor_->reset(cursor_);
	assert(ret == 0);
	ret = cursor_->next(cursor_);
	if (ret != 0) {
		valid_ = false;
		return;
	}
	ret = cursor_->get_key(cursor_, &item);
	assert(ret == 0);
	key_ = Slice((const char *)item.data, item.size);
	ret = cursor_->get_value(cursor_, &item);
	assert(ret == 0);
	value_ = Slice((const char *)item.data, item.size);
	valid_ = true;
}

// Position at the last key in the source.  The iterator is
// Valid() after this call iff the source is not empty.
void
IteratorImpl::SeekToLast()
{
	WT_ITEM item;

	int ret = cursor_->reset(cursor_);
	assert(ret == 0);
	ret = cursor_->prev(cursor_);
	if (ret != 0) {
		valid_ = false;
		return;
	}
	ret = cursor_->get_key(cursor_, &item);
	assert(ret == 0);
	key_ = Slice((const char *)item.data, item.size);
	ret = cursor_->get_value(cursor_, &item);
	assert(ret == 0);
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

	item.data = target.data();
	item.size = target.size();
	cursor_->set_key(cursor_, &item);
	int cmp, ret = cursor_->search_near(cursor_, &cmp);
	if (ret == 0 && cmp < 0)
		ret = cursor_->next(cursor_);
	if (ret == WT_NOTFOUND)
		status_ = Status::NotFound("Iterator::Seek key not found");
	if (ret != 0) {
		valid_ = false;
		return;
	}
	ret = cursor_->get_key(cursor_, &item);
	assert(ret == 0);
	key_ = Slice((const char *)item.data, item.size);
	ret = cursor_->get_value(cursor_, &item);
	assert(ret == 0);
	value_ = Slice((const char *)item.data, item.size);
	valid_ = true;
}

// Moves to the next entry in the source.  After this call, Valid() is
// true iff the iterator was not positioned at the last entry in the source.
// REQUIRES: Valid()
void
IteratorImpl::Next()
{
	WT_ITEM item;

	assert(valid_);

	int ret = cursor_->next(cursor_);
	if (ret == WT_NOTFOUND)
		status_ = Status::NotFound("Iterator::Next no more records");
	if (ret != 0) {
		valid_ = false;
		return;
	}
	ret = cursor_->get_key(cursor_, &item);
	assert(ret == 0);
	key_ = Slice((const char *)item.data, item.size);
	ret = cursor_->get_value(cursor_, &item);
	assert(ret == 0);
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

	assert(valid_);

	int ret = cursor_->prev(cursor_);
	if (ret == WT_NOTFOUND)
		status_ = Status::NotFound("Iterator::Prev no more records");
	if (ret != 0) {
		valid_ = false;
		return;
	}
	ret = cursor_->get_key(cursor_, &item);
	assert(ret == 0);
	key_ = Slice((const char *)item.data, item.size);
	ret = cursor_->get_value(cursor_, &item);
	assert(ret == 0);
	value_ = Slice((const char *)item.data, item.size);
	valid_ = true;
}
