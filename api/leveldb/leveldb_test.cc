/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * 	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
  assert(s.ok());

  leveldb::ReadOptions read_options;
  read_options.snapshot = db->GetSnapshot();
  leveldb::Iterator* iter = db->NewIterator(read_options);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    cout << iter->key().ToString() << ": "  << iter->value().ToString() << endl;
  }

  delete iter;
  db->ReleaseSnapshot(read_options.snapshot);

  delete db;

  return (0);
}
