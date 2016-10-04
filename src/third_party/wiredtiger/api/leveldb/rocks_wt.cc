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

static int
wtrocks_get_cursor(OperationContext *context, ColumnFamilyHandle *cfhp, WT_CURSOR **cursorp, int acquire=0)
{
	ColumnFamilyHandleImpl *cf =
	    static_cast<ColumnFamilyHandleImpl *>(cfhp);
	if (cf == NULL) {
		fprintf(stderr, "Missing column!\n");
		assert(0);
	}
	WT_CURSOR *c = context->GetCursor(cf->GetID());
	if (c == NULL) {
		WT_SESSION *session = context->GetSession();
		int ret;
		if ((ret = session->open_cursor(
		    session, cf->GetURI().c_str(), NULL, NULL, &c)) != 0) {
			fprintf(stderr, "Failed to open cursor on %s: %s\n", cf->GetURI().c_str(), wiredtiger_strerror(ret));
			return (ret);
		}
		if (!acquire)
			context->SetCursor(cf->GetID(), c);
	} else if (acquire)
		context->SetCursor(cf->GetID(), NULL);
	*cursorp = c;
	return (0);
}

Status
DB::ListColumnFamilies(
	Options const &options, std::string const &name,
	std::vector<std::string> *column_families)
{
	std::vector<std::string> cf;
	DB *dbptr;
	Status status = DB::Open(options, name, &dbptr);
	if (!status.ok())
		return status;
	DbImpl *db = static_cast<DbImpl *>(dbptr);
	OperationContext *context = db->GetContext();
	WT_SESSION *session = context->GetSession();
	WT_CURSOR *c;
	int ret = session->open_cursor(session, "metadata:", NULL, NULL, &c);
	if (ret != 0)
		goto err;
	c->set_key(c, "table:");
	/* Position on the first table entry */
	int cmp;
	ret = c->search_near(c, &cmp);
	if (ret != 0 || (cmp < 0 && (ret = c->next(c)) != 0))
		goto err;
	/* Add entries while we are getting "table" URIs. */
	for (; ret == 0; ret = c->next(c)) {
		const char *key;
		if ((ret = c->get_key(c, &key)) != 0)
			goto err;
		if (strncmp(key, "table:", strlen("table:")) != 0)
			break;
		printf("List column families: [%d] = %s\n", (int)cf.size(), key);
		cf.push_back(std::string(key + strlen("table:")));
	}

err:	delete db;
	/*
	 * WT_NOTFOUND is not an error: it just means we got to the end of the
	 * list of tables.
	 */
	if (ret == 0 || ret == WT_NOTFOUND) {
		*column_families = cf;
		ret = 0;
	}
	return WiredTigerErrorToStatus(ret);
}

Status
DB::Open(Options const &options, std::string const &name, const std::vector<ColumnFamilyDescriptor> &column_families, std::vector<ColumnFamilyHandle*> *handles, DB**dbptr)
{
	Status status = Open(options, name, dbptr);
	if (!status.ok())
		return status;
	DbImpl *db = static_cast<DbImpl *>(*dbptr);
	std::vector<ColumnFamilyHandle*> cfhandles(
	    column_families.size());
	for (size_t i = 0; i < column_families.size(); i++) {
		printf("Open column families: [%d] = %s\n", (int)i, column_families[i].name.c_str());
		cfhandles[i] = new ColumnFamilyHandleImpl(
		    db, column_families[i].name, (int)i);
	}
	db->SetColumns(*handles = cfhandles);
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
WriteBatchHandler::PutCF(
    uint32_t column_family_id, const Slice& key, const Slice& value)
{
	WT_CURSOR *cursor;
	int ret = wtrocks_get_cursor(context_, db_->GetCF(column_family_id), &cursor);
	if (ret != 0)
		return WiredTigerErrorToStatus(ret);
	WT_ITEM item;
	item.data = key.data();
	item.size = key.size();
	cursor->set_key(cursor, &item);
	item.data = value.data();
	item.size = value.size();
	cursor->set_value(cursor, &item);
	ret = cursor->insert(cursor);
	return WiredTigerErrorToStatus(ret);
}

Status
WriteBatchHandler::DeleteCF(uint32_t column_family_id, const Slice& key)
{
	WT_CURSOR *cursor;
	int ret = wtrocks_get_cursor(context_, db_->GetCF(column_family_id), &cursor);
	if (ret != 0)
		return WiredTigerErrorToStatus(ret);
	WT_ITEM item;
	item.data = key.data();
	item.size = key.size();
	cursor->set_key(cursor, &item);
	ret = cursor->remove(cursor);
	if (ret == 0) {
		int t_ret = cursor->reset(cursor);
		assert(t_ret == 0);
	} else if (ret == WT_NOTFOUND)
		ret = 0;
	return WiredTigerErrorToStatus(ret);
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
	int id = (int)columns_.size();
	*cfhp = new ColumnFamilyHandleImpl(this, name, id);
	printf("Create column family: [%d] = %s\n", id, name.c_str());
	columns_.push_back(*cfhp);
	return Status::OK();
}

Status
DbImpl::DropColumnFamily(ColumnFamilyHandle *cfhp)
{
	ColumnFamilyHandleImpl *cf =
	    static_cast<ColumnFamilyHandleImpl *>(cfhp);
	WT_SESSION *session = GetContext()->GetSession();
	int ret = session->drop(session, cf->GetURI().c_str(), NULL);
	return WiredTigerErrorToStatus(ret);
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
	cursor->set_key(cursor, &item);
	ret = cursor->remove(cursor);
	// Reset the WiredTiger cursor so it doesn't keep any pages pinned.
	// Track failures in debug builds since we don't expect failure, but
	// don't pass failures on - it's not necessary for correct operation.
	int t_ret = cursor->reset(cursor);
	assert(t_ret == 0);
	return WiredTigerErrorToStatus(ret);
}

Status
DbImpl::Flush(FlushOptions const&, ColumnFamilyHandle* cfhp)
{
	ColumnFamilyHandleImpl *cf =
	    static_cast<ColumnFamilyHandleImpl *>(cfhp);
	WT_SESSION *session = GetContext()->GetSession();
	return WiredTigerErrorToStatus(session->checkpoint(session, ("target=(\"" + cf->GetURI() + "\")").c_str()));
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
	    (ret = cursor->get_value(cursor, &item)) == 0) {
		*value = std::string((const char *)item.data, item.size);
		ret = cursor->reset(cursor);
	}
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

	WT_CURSOR *c;
	int ret = wtrocks_get_cursor(context, cfhp, &c, 1);
	assert(ret == 0);
	return new IteratorImpl(this, c,
	    static_cast<ColumnFamilyHandleImpl *>(cfhp)->GetID());
}

Status
DbImpl::Put(WriteOptions const &options, ColumnFamilyHandle *cfhp, Slice const &key, Slice const &value)
{
	WT_CURSOR *cursor;
	int ret = wtrocks_get_cursor(GetContext(), cfhp, &cursor);
	if (ret != 0)
		return WiredTigerErrorToStatus(ret);

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
