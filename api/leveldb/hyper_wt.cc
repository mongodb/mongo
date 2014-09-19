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
#include <sys/param.h>
#include <sys/stat.h>

using leveldb::ReplayIterator;
using leveldb::Status;

// Fill in missing methods from the interface
ReplayIterator::ReplayIterator() {}
ReplayIterator::~ReplayIterator() {}

class ReplayIteratorImpl : public ReplayIterator {
 public:
  ReplayIteratorImpl(OperationContext *context) : context_(context), cursor_(NULL) {
    WT_SESSION *session = context_->GetSession();
    int ret = session->open_cursor(
        session, "log:", NULL, NULL, &cursor_);
    status_ = WiredTigerErrorToStatus(ret);
    valid_ = false;
    // Position on first record.  valid_ will be set appropriately.
    Next();
  }

  ReplayIteratorImpl(OperationContext *context, const std::string& timestamp) :
    context_(context), cursor_(NULL) {

    WT_SESSION *session = context_->GetSession();
    int ret = session->open_cursor(
        session, "log:", NULL, NULL, &cursor_);
    status_ = WiredTigerErrorToStatus(ret);
    valid_ = false;
    // Position on requested record.  valid_ will be set appropriately.
    SeekTo(timestamp);
  }

  // An iterator is either positioned at a deleted key, present key/value pair,
  // or not valid.  This method returns true iff the iterator is valid.
  virtual bool Valid();

  // Moves to the next entry in the source.  After this call, Valid() is
  // true iff the iterator was not positioned at the last entry in the source.
  // REQUIRES: Valid()
  virtual void Next();

  // Position at the first key in the source that at or past target for this
  // pass.  Note that this is unlike the Seek call, as the ReplayIterator is
  // unsorted.
  // The iterator is Valid() after this call iff the source contains
  // an entry that comes at or past target.
  // Per Robert at Hyperdex, the SkipTo functions are hacky optimizations
  // for LevelDB and its key layout.  It is okay for them to be no-ops. 
  virtual void SkipTo(const Slice& target) { }
  virtual void SkipToLast() { }
  virtual void SeekTo(const std::string& timestamp);
  virtual void SeekToLast();

  // Return true if the current entry points to a key-value pair.  If this
  // returns false, it means the current entry is a deleted entry.
  virtual bool HasValue() { 
    assert(Valid());
    if (optype == WT_LOGOP_ROW_PUT ||
	optype == WT_LOGOP_COL_PUT)
	  return true;
    else
	  return false;
  }

  int Compare(ReplayIteratorImpl* other) {
    int cmp;
    assert(Valid());
    // assert(other->Valid());
    int ret = cursor_->compare(cursor_, other->cursor_, &cmp);
    status_ = WiredTigerErrorToStatus(ret);
    return (cmp);
  }

  // Return the key for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  virtual Slice key() const { return Slice((const char *)key_.data, key_.size); }

  // Return the value for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: !AtEnd() && !AtStart()
  virtual Slice value() const { return Slice((const char *)value_.data, value_.size); }

  // If an error has occurred, return it.  Else return an ok status.
  virtual Status status() const { return status_; }

  // must be released by giving it back to the DB
  virtual ~ReplayIteratorImpl() {
    int ret = Close();
    assert(ret == 0);
  }

  std::string GetTimestamp() {
	char lsn[256];
        assert(Valid());
	snprintf(lsn, sizeof(lsn), WT_TIMESTAMP_FORMAT,
	    lsn_.file, lsn_.offset);
	return (std::string(lsn));
  }

  int Close() {
    int ret = 0;
    if (cursor_ != NULL)
      ret = cursor_->close(cursor_);
    status_ = WiredTigerErrorToStatus(ret);
    valid_ = false;
    cursor_ = NULL;
    return (ret);
  }

 private:
  void SeekTo(WT_LSN *lsn);
  // No copying allowed
  ReplayIteratorImpl(const ReplayIterator&) { }
  void operator=(const ReplayIterator&) { }
  OperationContext *context_;
  Status status_;
  WT_CURSOR *cursor_;
  WT_ITEM key_, value_;
  WT_LSN lsn_;
  bool valid_;
  uint64_t txnid;
  uint32_t fileid, opcount, optype, rectype;
};

bool
ReplayIteratorImpl::Valid() {
	// If we're invalid and at the end, try again.
	if (valid_ == false && cursor_ != NULL && status_.IsNotFound())
		Next();
	return valid_;
}

void
ReplayIteratorImpl::Next() {
	int ret = 0;

	if (cursor_ != NULL) {
		while ((ret = cursor_->next(cursor_)) == 0) {
			ret = cursor_->get_key(cursor_,
			    &lsn_.file, &lsn_.offset, &opcount);
			if (ret != 0)
				break;
			ret = cursor_->get_value(cursor_,
			    &txnid, &rectype, &optype, &fileid, &key_, &value_);
			if (ret != 0)
				break;
			// Next() is only interested in modification operations.
			// Continue for any other type of record.
			if (WT_VALID_OPERATION(fileid, optype)) {
				valid_ = true;
				break;
			}
		}
		status_ = WiredTigerErrorToStatus(ret);
		if (ret != 0) {
			valid_ = false;
			if (ret != WT_NOTFOUND)
				ret = Close();
			else
				ret = 0;
			assert(ret == 0);
		}
	}
}

void
ReplayIteratorImpl::SeekToLast() {
	int ret = 0;
	WT_LSN last_lsn;

	last_lsn.file = 0;
	if (cursor_ != NULL) {
		// Walk the log to the end, then set the cursor on the
		// last valid LSN we saw.
		while ((ret = cursor_->next(cursor_)) == 0) {
			ret = cursor_->get_key(cursor_,
			    &lsn_.file, &lsn_.offset, &opcount);
			if (ret != 0)
				break;
			ret = cursor_->get_value(cursor_,
			    &txnid, &rectype, &optype, &fileid, &key_, &value_);
			if (ret != 0)
				break;
			// We're only interested in modification operations.
			// Continue for any other type of record.
			if (WT_VALID_OPERATION(fileid, optype)) {
				valid_ = true;
				last_lsn = lsn_;
			}
		}
		// We reached the end of log
		if (ret != WT_NOTFOUND || last_lsn.file == 0) {
			valid_ = false;
			ret = Close();
			assert(ret == 0);
		} else
			SeekTo(&last_lsn);
	}
}

void
ReplayIteratorImpl::SeekTo(const std::string& timestamp) {
	WT_LSN target_lsn;
	int ret = 0;

	if (timestamp == "all") {
		if (cursor_ != NULL) {
			ret = cursor_->reset(cursor_);
			status_ = WiredTigerErrorToStatus(ret);
			if (ret != 0)
				return;
			Next();
			return;
		}
	}
	if (timestamp == "now") {
		SeekToLast();
		return;
	}
	sscanf(timestamp.c_str(), WT_TIMESTAMP_FORMAT,
	    &target_lsn.file, &target_lsn.offset);
	SeekTo(&target_lsn);
}

// Set the cursor on the first modification record at or after the
// given LSN.
void
ReplayIteratorImpl::SeekTo(WT_LSN *target_lsn) {
	int ret = 0;

	valid_ = false;
	if (cursor_ != NULL) {
		cursor_->set_key(cursor_,
		    target_lsn->file, target_lsn->offset, 0, 0);
		ret = cursor_->search(cursor_);
		status_ = WiredTigerErrorToStatus(ret);
		if (ret != 0)
			return;
		// If we were successful, set up the info.
		ret = cursor_->get_key(cursor_,
		    &lsn_.file, &lsn_.offset, &opcount);
		status_ = WiredTigerErrorToStatus(ret);
		if (ret != 0)
			return;
		ret = cursor_->get_value(cursor_,
		    &txnid, &rectype, &optype, &fileid, &key_, &value_);
		status_ = WiredTigerErrorToStatus(ret);
		if (ret != 0)
			return;
		valid_ = true;
		// We're only interested in modification operations.
		// Continue for any other type of record.
		if (!WT_VALID_OPERATION(fileid, optype))
			Next();
	}
}

// Create a live backup of a live LevelDB instance.
// The backup is stored in a directory named "backup-<name>" under the top
// level of the open LevelDB database.  The implementation is permitted, and
// even encouraged, to improve the performance of this call through
// hard-links.
Status
DbImpl::LiveBackup(const Slice& name)
{
	OperationContext *context = GetContext();
	WT_SESSION *session = context->GetSession();
	WT_CURSOR *cursor;
	int ret = session->open_cursor(
            session, "backup:", NULL, NULL, &cursor);
	int t_ret;
	const char *filename;
	const char *home = conn_->get_home(conn_);
	char backup[MAXPATHLEN], buf[MAXPATHLEN * 2];
 
	// If we couldn't open the backup cursor, we're done.
	if (ret != 0)
		return (WiredTigerErrorToStatus(ret));

	// Remove any old directory and create the backup directory.
	// WT single-threads hot backups.  If we get here we already have
	// the backup cursor open and we do not have to worry about other
	// threads trying to remove and recreate the same directory out
	// from under us.
	snprintf(buf, sizeof(buf), "rm -rf %s/backup-%s", home,
	    (char *)name.data());
	if ((ret = system(buf)) != 0)
		return WiredTigerErrorToStatus(ret);
	snprintf(backup, sizeof(backup), "%s/backup-%s", home,
	    (char *)name.data());
	if ((ret = mkdir(backup, 0777)) != 0)
		return WiredTigerErrorToStatus(ret);
	// Copy all files returned by backup cursor.
	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &filename)) == 0) {
		snprintf(buf, sizeof(buf), "cp %s/%s %s/%s",
		    home, filename, backup, filename);
		if ((ret = system(buf)) != 0)
			break;
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	if ((t_ret = cursor->close(cursor)) != 0 && ret == 0)
		ret = t_ret;

	return (WiredTigerErrorToStatus(ret));
}

// Return an opaque timestamp that identifies the current point in time of the
// database.  This timestamp may be subsequently presented to the
// NewReplayIterator method to create a ReplayIterator.
void
DbImpl::GetReplayTimestamp(std::string* timestamp)
{
	OperationContext *context = GetContext();
	ReplayIteratorImpl *iter = new ReplayIteratorImpl(context);

	iter->SeekToLast();
	*timestamp = iter->GetTimestamp();
	ReleaseReplayIterator(iter);
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
	bool valid;
	OperationContext *context = GetContext();
	ReplayIteratorImpl *iter = new ReplayIteratorImpl(context);

	// The SeekTo function will handle "all" or "now".
	iter->SeekTo(timestamp);
	valid = iter->Valid();
	ReleaseReplayIterator(iter);
	return valid;
}

// Compare two timestamps and return -1, 0, 1 for lt, eq, gt
int
DbImpl::CompareTimestamps(const std::string& lhs, const std::string& rhs)
{
	OperationContext *context = GetContext();
	ReplayIteratorImpl *lhiter = new ReplayIteratorImpl(context);
	ReplayIteratorImpl *rhiter = new ReplayIteratorImpl(context);
	int cmp = 0;
	
	// The SeekTo function will handle "all" or "now".
	lhiter->SeekTo(lhs);
	rhiter->SeekTo(rhs);
	if (lhiter->Valid() && rhiter->Valid())
		cmp = lhiter->Compare(rhiter);
	ReleaseReplayIterator(lhiter);
	ReleaseReplayIterator(rhiter);
	return cmp;
}

// Return a ReplayIterator that returns every write operation performed after
// the timestamp.
Status
DbImpl::GetReplayIterator(const std::string& timestamp,
			   ReplayIterator** iter)
{
	OperationContext *context = GetContext();
	*iter = new ReplayIteratorImpl(context, timestamp);
	return ((*iter)->status());
}

// Release a previously allocated replay iterator.
void
DbImpl::ReleaseReplayIterator(ReplayIterator* iter)
{
	delete static_cast<ReplayIteratorImpl *>(iter);
}
