/*-
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

#include "leveldb_wt.h"
#include <errno.h>
#include <sstream>

using leveldb::Status;
using leveldb::ReplayIterator;

// Fill in missing methods from the interface
ReplayIterator::ReplayIterator() {}
ReplayIterator::~ReplayIterator() {}

class ReplayIteratorImpl : public ReplayIterator {
 public:
  ReplayIteratorImpl() {}

  // An iterator is either positioned at a deleted key, present key/value pair,
  // or not valid.  This method returns true iff the iterator is valid.
  virtual bool Valid() { return false; }

  // Moves to the next entry in the source.  After this call, Valid() is
  // true iff the iterator was not positioned at the last entry in the source.
  // REQUIRES: Valid()
  virtual void Next() { }

  // Position at the first key in the source that at or past target for this
  // pass.  Note that this is unlike the Seek call, as the ReplayIterator is
  // unsorted.
  // The iterator is Valid() after this call iff the source contains
  // an entry that comes at or past target.
  virtual void SkipTo(const Slice& target) { }
  virtual void SkipToLast() { }

  // Return true if the current entry points to a key-value pair.  If this
  // returns false, it means the current entry is a deleted entry.
  virtual bool HasValue() { return false; }

  // Return the key for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  virtual Slice key() const { return Slice(); }

  // Return the value for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: !AtEnd() && !AtStart()
  virtual Slice value() const { return Slice(); }

  // If an error has occurred, return it.  Else return an ok status.
  virtual Status status() const { return Status::NotSupported("ReplayIterator"); }

  // must be released by giving it back to the DB
  virtual ~ReplayIteratorImpl() { }

 private:
  // No copying allowed
  ReplayIteratorImpl(const ReplayIterator&) { }
  void operator=(const ReplayIterator&) { }
};

// Create a live backup of a live LevelDB instance.
// The backup is stored in a directory named "backup-<name>" under the top
// level of the open LevelDB database.  The implementation is permitted, and
// even encouraged, to improve the performance of this call through
// hard-links.
Status
DbImpl::LiveBackup(const Slice& name)
{
	return Status::NotSupported("DB::LiveBackup");
}

// Return an opaque timestamp that identifies the current point in time of the
// database.  This timestamp may be subsequently presented to the
// NewReplayIterator method to create a ReplayIterator.
void
DbImpl::GetReplayTimestamp(std::string* timestamp)
{
	*timestamp = std::string("current lsn");
}

// Set the lower bound for manual garbage collection.  This method only takes
// effect when Options.manual_garbage_collection is true.
void
DbImpl::AllowGarbageCollectBeforeTimestamp(const std::string& timestamp)
{
}

// Validate the timestamp
bool
DbImpl::ValidateTimestamp(const std::string& timestamp)
{
	return false;
}

// Compare two timestamps and return -1, 0, 1 for lt, eq, gt
int
DbImpl::CompareTimestamps(const std::string& lhs, const std::string& rhs)
{
	return 0;
}

// Return a ReplayIterator that returns every write operation performed after
// the timestamp.
Status
DbImpl::GetReplayIterator(const std::string& timestamp,
			   ReplayIterator** iter)
{
	*iter = new ReplayIteratorImpl();
	return Status::NotSupported("DB::GetReplayIterator");
}

// Release a previously allocated replay iterator.
void
DbImpl::ReleaseReplayIterator(ReplayIterator* iter)
{
	delete static_cast<ReplayIteratorImpl *>(iter);
}
