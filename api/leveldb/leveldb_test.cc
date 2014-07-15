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
