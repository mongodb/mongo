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

#include <assert.h>
#include <iostream>
#include "leveldb_wt.h"

using namespace std;

extern "C" int main() {
  leveldb::DB* db;
  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::Status s = leveldb::DB::Open(options, "WTLDB_HOME", &db);
  assert(s.ok());

  s = db->Put(leveldb::WriteOptions(), "key", "value");
  s = db->Put(leveldb::WriteOptions(), "key2", "value2");
  s = db->Put(leveldb::WriteOptions(), "key3", "value3");
  s = db->Put(leveldb::WriteOptions(), "key4", "value4");
  assert(s.ok());

#ifdef	HAVE_HYPERLEVELDB
  leveldb::ReplayIterator* replay_start;
  leveldb::ReplayIterator* replay_ts;
  leveldb::ReplayIterator* replay_now;
  leveldb::ReplayIterator* replay_last;
  std::string timestamp;
  std::string timestamp_last;

  cout << "Perform Live Backup" << endl;
  s = db->LiveBackup("test");

  // Test out a bunch of the ReplayIterator methods.
  db->GetReplayTimestamp(&timestamp);
  cout << "timestamp 1 " << timestamp << endl << "Put key5" << endl;
  s = db->Put(leveldb::WriteOptions(), "key5", "value5");
  db->GetReplayTimestamp(&timestamp_last);
  // Verify a bunch of timestamp comparisons
  cout << "timestamp 2 " << timestamp_last << endl;
  cout << "CompareTimestamps tests" << endl;
  assert(db->CompareTimestamps(timestamp, timestamp_last) < 0);
  assert(db->CompareTimestamps("all", timestamp_last) < 0);
  assert(db->CompareTimestamps(timestamp, "now") < 0);
  assert(db->CompareTimestamps("now", timestamp_last) == 0);
  assert(db->CompareTimestamps(timestamp_last, "now") == 0);
  assert(db->CompareTimestamps("now", timestamp) > 0);
  assert(db->CompareTimestamps("now", "all") > 0);

  s = db->GetReplayIterator("all", &replay_start);
  assert(replay_start->Valid());
  cout << "Replay at all(start):" << endl;
  cout << replay_start->key().ToString() << ": " << replay_start->value().ToString() << endl;
  s = db->GetReplayIterator(timestamp, &replay_ts);
  assert(replay_ts->Valid());
  cout << "Replay at timestamp " << timestamp << ":" << endl;
  cout << replay_ts->key().ToString() << ": " << replay_ts->value().ToString() << endl;
  s = db->GetReplayIterator("now", &replay_now);
  assert(replay_now->Valid());
  cout << "Replay at now(end):" << endl;
  cout << replay_now->key().ToString() << ": " << replay_now->value().ToString() << endl;
  s = db->GetReplayIterator(timestamp_last, &replay_last);
  assert(replay_last->Valid());
  cout << "Replay at last timestamp " << timestamp_last << ":" << endl;
  cout << replay_last->key().ToString() << ": " << replay_last->value().ToString() << endl;
  assert(replay_now->key().ToString() == replay_last->key().ToString());
  cout << "Replay walk from all/start:" << endl;
  while (replay_start->Valid()) {
    cout << replay_start->key().ToString() << ": " << replay_start->value().ToString() << endl;
    replay_start->Next();
  }
  // We reached the end of log, iterator should still not be valid.
  // But if we write something, the iterator should find it and become
  // valid again.
  assert(!replay_start->Valid());
  s = db->Put(leveldb::WriteOptions(), "key6", "value6");
  assert(replay_start->Valid());
  db->ReleaseReplayIterator(replay_start);
  db->ReleaseReplayIterator(replay_ts);
  db->ReleaseReplayIterator(replay_now);
  db->ReleaseReplayIterator(replay_last);
#endif

  // Read through the main database
  cout << "Read main database:" << endl;
  leveldb::ReadOptions read_options;
  read_options.snapshot = db->GetSnapshot();
  leveldb::Iterator* iter = db->NewIterator(read_options);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    cout << iter->key().ToString() << ": "  << iter->value().ToString() << endl;
  }

  delete iter;
  db->ReleaseSnapshot(read_options.snapshot);

  delete db;

#ifdef	HAVE_HYPERLEVELDB
  // Read through the backup database
  leveldb::DB* db_bkup;
  options.create_if_missing = false;
  s = leveldb::DB::Open(options, "WTLDB_HOME/backup-test", &db_bkup);
  read_options.snapshot = db_bkup->GetSnapshot();
  iter = db_bkup->NewIterator(read_options);
  cout << "Read Backup database:" << endl;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    cout << iter->key().ToString() << ": "  << iter->value().ToString() << endl;
  }

  delete iter;
  db_bkup->ReleaseSnapshot(read_options.snapshot);

  delete db_bkup;
#endif

  return (0);
}
