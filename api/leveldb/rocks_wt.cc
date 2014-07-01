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
using leveldb::DB;
using leveldb::FlushOptions;
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
DB::ListColumnFamilies(Options const &, std::string const &, std::vector<std::string> *)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DB::Open(Options const &options, std::string const &name, const std::vector<ColumnFamilyDescriptor> &column_families, std::vector<ColumnFamilyHandle*> *handles, DB**dbptr)
{
	Status status = Open(options, name, dbptr);
	if (!status.ok())
		return status;
	DbImpl *db = reinterpret_cast<DbImpl *>(dbptr);
	std::vector<ColumnFamilyHandle*> cfhandles(
	    column_families.size());
	for (size_t i = 0; i < column_families.size(); i++)
		cfhandles[i] = new ColumnFamilyHandleImpl(
		    db, column_families[i].name, (int)i);
	*handles = cfhandles;
	return Status::OK();
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
DbImpl::Merge(WriteOptions const&, ColumnFamilyHandle*, Slice const&, Slice const&)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DbImpl::CreateColumnFamily(Options const &options, std::string const &name, ColumnFamilyHandle **cfhp)
{
	extern int wtleveldb_create(WT_CONNECTION *,
	    const Options &, std::string const &uri);
	int ret = wtleveldb_create(conn_, options, "table:" + name);
	if (ret != 0)
		return WiredTigerErrorToStatus(ret);
	*cfhp = new ColumnFamilyHandleImpl(this, name, ++numColumns_);
	return Status::OK();
}

Status
DbImpl::DropColumnFamily(ColumnFamilyHandle *cfhp)
{
	ColumnFamilyHandleImpl *cf =
	    reinterpret_cast<ColumnFamilyHandleImpl *>(cfhp);
	WT_SESSION *session = GetContext()->GetSession();
	int ret = session->drop(session, cf->GetURI().c_str(), NULL);
	return WiredTigerErrorToStatus(ret);
}

static int
wtrocks_get_cursor(OperationContext *context, ColumnFamilyHandle *cfhp, WT_CURSOR **cursorp)
{
	ColumnFamilyHandleImpl *cf =
	    reinterpret_cast<ColumnFamilyHandleImpl *>(cfhp);
	WT_CURSOR *c = context->GetCursor(cf->GetID());
	if (c == NULL) {
		WT_SESSION *session = context->GetSession();
		int ret;
		if ((ret = session->open_cursor(
		    session, cf->GetURI().c_str(), NULL, NULL, &c)) != 0)
			return (ret);
		context->SetCursor(cf->GetID(), c);
	}
	*cursorp = c;
	return (0);
}

Status
DbImpl::Delete(WriteOptions const &write_options, ColumnFamilyHandle *cfhp, Slice const &key)
{
	WT_CURSOR *cursor;
	int ret = wtrocks_get_cursor(GetContext(), cfhp, &cursor);
	if (ret != 0)
		return WiredTigerErrorToStatus(ret);
	WT_ITEM item;
	item.data = key.data();
	item.size = key.size();
	ret = cursor->remove(cursor);
	// Reset the WiredTiger cursor so it doesn't keep any pages pinned.
	// Track failures in debug builds since we don't expect failure, but
	// don't pass failures on - it's not necessary for correct operation.
	int t_ret = cursor->reset(cursor);
	assert(t_ret == 0);
	return WiredTigerErrorToStatus(ret);
}

Status
DbImpl::Flush(FlushOptions const&, ColumnFamilyHandle*)
{
	return WiredTigerErrorToStatus(ENOTSUP);
}

Status
DbImpl::Get(ReadOptions const &options, ColumnFamilyHandle *cfhp, Slice const &key, std::string *value)
{
	const char *errmsg = NULL;
	OperationContext *context = GetContext(options);

	WT_CURSOR *cursor;
	int ret = wtrocks_get_cursor(context, cfhp, &cursor);
	if (ret != 0)
		return WiredTigerErrorToStatus(ret);

	WT_ITEM item;
	item.data = key.data();
	item.size = key.size();
	cursor->set_key(cursor, &item);
	if ((ret = cursor->search(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &item)) == 0)
		*value = std::string((const char *)item.data, item.size);
	if (ret == WT_NOTFOUND)
		errmsg = "DB::Get key not found";
	return WiredTigerErrorToStatus(ret, errmsg);
}

bool
DbImpl::GetProperty(ColumnFamilyHandle*, Slice const&, std::string*)
{
	return false;
}

std::vector<Status>
DbImpl::MultiGet(ReadOptions const&, std::vector<ColumnFamilyHandle*> const&, std::vector<Slice> const&, std::vector<std::string, std::allocator<std::string> >*)
{
	std::vector<Status> ret;
	ret.push_back(WiredTigerErrorToStatus(ENOTSUP));
	return ret;
}

Iterator *
DbImpl::NewIterator(ReadOptions const &options, ColumnFamilyHandle *cfhp)
{
	OperationContext *context = GetContext(options);

	/* Duplicate the normal cursor for the iterator. */
	WT_SESSION *session = context->GetSession();
	WT_CURSOR *c, *iterc;
	int ret = wtrocks_get_cursor(context, cfhp, &c);
	assert(ret == 0);
	ret = session->open_cursor(session, NULL, c, NULL, &iterc);
	assert(ret == 0);
	return new IteratorImpl(this, iterc);
}

Status
DbImpl::Put(WriteOptions const &options, ColumnFamilyHandle *cfhp, Slice const &key, Slice const &value)
{
	WT_CURSOR *cursor;
	int ret = wtrocks_get_cursor(GetContext(), cfhp, &cursor);
	WT_ITEM item;

	item.data = key.data();
	item.size = key.size();
	cursor->set_key(cursor, &item);
	item.data = value.data();
	item.size = value.size();
	cursor->set_value(cursor, &item);
	ret = cursor->insert(cursor);
	return WiredTigerErrorToStatus(ret, NULL);
}
