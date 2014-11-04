// Copyright (c) 2013 The HyperLevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_REPLAY_ITERATOR_H_
#define STORAGE_LEVELDB_INCLUDE_REPLAY_ITERATOR_H_

#include "leveldb_wt_config.h"

#include "slice.h"
#include "status.h"

namespace leveldb {

class ReplayIterator {
 public:
  ReplayIterator();

  // An iterator is either positioned at a deleted key, present key/value pair,
  // or not valid.  This method returns true iff the iterator is valid.
  virtual bool Valid() = 0;

  // Moves to the next entry in the source.  After this call, Valid() is
  // true iff the iterator was not positioned at the last entry in the source.
  // REQUIRES: Valid()
  virtual void Next() = 0;

  // Position at the first key in the source that at or past target for this
  // pass.  Note that this is unlike the Seek call, as the ReplayIterator is
  // unsorted.
  // The iterator is Valid() after this call iff the source contains
  // an entry that comes at or past target.
  virtual void SkipTo(const Slice& target) = 0;
  virtual void SkipToLast() = 0;

  // Return true if the current entry points to a key-value pair.  If this
  // returns false, it means the current entry is a deleted entry.
  virtual bool HasValue() = 0;

  // Return the key for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  virtual Slice key() const = 0;

  // Return the value for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: !AtEnd() && !AtStart()
  virtual Slice value() const = 0;

  // If an error has occurred, return it.  Else return an ok status.
  virtual Status status() const = 0;

 protected:
  // must be released by giving it back to the DB
  virtual ~ReplayIterator();

 private:
  // No copying allowed
  ReplayIterator(const ReplayIterator&);
  void operator=(const ReplayIterator&);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_REPLAY_ITERATOR_H_
