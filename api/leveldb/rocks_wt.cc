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

Status
rocksdb::DB::ListColumnFamilies(rocksdb::Options const&, std::string const&, std::vector<std::string, std::allocator<std::string> >*)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
rocksdb::DB::Open(rocksdb::Options const&, std::string const&, std::vector<rocksdb::ColumnFamilyDescriptor, std::allocator<rocksdb::ColumnFamilyDescriptor> > const&, std::vector<rocksdb::ColumnFamilyHandle*, std::allocator<rocksdb::ColumnFamilyHandle*> >*, rocksdb::DB**)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

void
WriteBatch::Handler::Merge(const Slice& key, const Slice& value)
{
}

void
WriteBatch::Handler::LogData(const Slice& blob)
{
}

Status
DbImpl::Merge(rocksdb::WriteOptions const&, rocksdb::ColumnFamilyHandle*, rocksdb::Slice const&, rocksdb::Slice const&)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DbImpl::CreateColumnFamily(rocksdb::Options const&, std::string const&, rocksdb::ColumnFamilyHandle**)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DbImpl::DropColumnFamily(rocksdb::ColumnFamilyHandle*)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DbImpl::Delete(rocksdb::WriteOptions const&, rocksdb::ColumnFamilyHandle*, rocksdb::Slice const&)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DbImpl::Flush(rocksdb::FlushOptions const&, rocksdb::ColumnFamilyHandle*)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DbImpl::Get(rocksdb::ReadOptions const&, rocksdb::ColumnFamilyHandle*, rocksdb::Slice const&, std::string*)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

bool
DbImpl::GetProperty(rocksdb::ColumnFamilyHandle*, rocksdb::Slice const&, std::string*)
{
	return false;
}

std::vector<Status>
DbImpl::MultiGet(rocksdb::ReadOptions const&, std::vector<rocksdb::ColumnFamilyHandle*, std::allocator<rocksdb::ColumnFamilyHandle*> > const&, std::vector<rocksdb::Slice, std::allocator<rocksdb::Slice> > const&, std::vector<std::string, std::allocator<std::string> >*)
{
	std::vector<Status> ret;
	ret.push_back(WiredTigerErrorToStatus(ENOTSUP));
	return ret;
}

Iterator *
DbImpl::NewIterator(rocksdb::ReadOptions const&, rocksdb::ColumnFamilyHandle*)
{
	return NULL;
}

Status
DbImpl::Put(rocksdb::WriteOptions const&, rocksdb::ColumnFamilyHandle*, rocksdb::Slice const&, rocksdb::Slice const&)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}
