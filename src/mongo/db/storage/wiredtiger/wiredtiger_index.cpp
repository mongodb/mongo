/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/db/catalog/index_catalog_entry.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"

namespace mongo {
    static const WiredTigerItem emptyItem(NULL, 0);
    
    bool hasFieldNames(const BSONObj& obj) {
        BSONForEach(e, obj) {
            if (e.fieldName()[0])
                return true;
        }
        return false;
    }

    BSONObj stripFieldNames(const BSONObj& query) {
        if (!hasFieldNames(query))
            return query;

        BSONObjBuilder bb;
        BSONForEach(e, query) {
            bb.appendAs(e, StringData());
        }
        return bb.obj();
    }

    WiredTigerItem _toItem( const BSONObj& key, bool removeFieldNames = true ) {
	const BSONObj& finalKey = removeFieldNames ? stripFieldNames( key ) : key;
	return WiredTigerItem( finalKey.objdata(), finalKey.objsize() );
    }

    WiredTigerItem _toItem( const DiskLoc& loc ) {
	return WiredTigerItem( reinterpret_cast<const char*>( &loc ), sizeof( loc ) );
    }

    // taken from btree_logic.cpp
    Status dupKeyError(const BSONObj& key) {
        StringBuilder sb;
        sb << "E11000 duplicate key error ";
        // sb << "index: " << _indexName << " "; // TODO
        sb << "dup key: " << key;
        return Status(ErrorCodes::DuplicateKey, sb.str());
    }

    int WiredTigerIndex::Create(WiredTigerDatabase &db, const std::string &ns, const std::string &idxName, IndexCatalogEntry& info) {
	WiredTigerSession swrap(db.GetSession(), db);
	WT_SESSION *s(swrap.Get());
	return s->create(s, _getURI(ns, idxName).c_str(), "type=file,key_format=uu,value_format=u");
    }

    Status WiredTigerIndex::insert(OperationContext* txn,
			  const BSONObj& key,
			  const DiskLoc& loc,
			  bool dupsAllowed) {
	invariant(!loc.isNull());
	invariant(loc.isValid());
	invariant(!hasFieldNames(key));

	// TODO optimization: save the iterator from the dup-check to speed up insert
	if (!dupsAllowed && isDup(txn, key, loc))
	    return dupKeyError(key);

	WiredTigerSession swrap(_db);
	WiredTigerCursor curwrap(GetCursor(swrap), swrap);
	WT_CURSOR *c = curwrap.Get();
	c->set_key(c, _toItem(key).Get(), _toItem(loc).Get());
	c->set_value(c, &emptyItem);
	int ret = c->insert(c);
	invariant(ret == 0);
	return Status::OK();
    }

    bool WiredTigerIndex::unindex(OperationContext* txn, const BSONObj& key, const DiskLoc& loc) {
	invariant(!loc.isNull());
	invariant(loc.isValid());
	invariant(!hasFieldNames(key));

	WiredTigerSession swrap(_db);
	WiredTigerCursor curwrap(GetCursor(swrap), swrap);
	WT_CURSOR *c = curwrap.Get();
	c->set_key(c, _toItem(key).Get(), _toItem(loc).Get());
	int ret = c->remove(c);
	invariant(ret == 0);
	// TODO: can we avoid a search?
	const size_t numDeleted = 1;
	return numDeleted == 1;
    }

    void WiredTigerIndex::fullValidate(OperationContext* txn, long long *numKeysOut) {
	// TODO check invariants?
	*numKeysOut = 1;
    }

    Status WiredTigerIndex::dupKeyCheck(OperationContext* txn, const BSONObj& key, const DiskLoc& loc) {
	invariant(!hasFieldNames(key));
	if (isDup(txn, key, loc))
	    return dupKeyError(key);
	return Status::OK();
    }

    bool WiredTigerIndex::isEmpty() {
	// XXX no context?
	WiredTigerSession swrap(_db);
	WiredTigerCursor curwrap(GetCursor(swrap), swrap);
	WT_CURSOR *c = curwrap.Get();
	if (!c)
		return true;
	int ret = c->next(c);
	if (ret == WT_NOTFOUND)
	    return true;
	invariant(ret == 0);
	return false;
    }

    Status WiredTigerIndex::touch(OperationContext* txn) const{
	// already in memory...
	return Status::OK();
    }
    
    long long WiredTigerIndex::getSpaceUsedBytes( OperationContext* txn ) const { return 1; }

    bool WiredTigerIndex::isDup(OperationContext *txn, const BSONObj& key, DiskLoc loc) {
	boost::scoped_ptr<SortedDataInterface::Cursor> cursor( newCursor( txn, 1 ) );
	cursor->locate(key, DiskLoc());

	return !cursor->isEOF() && cursor->getDiskLoc() == loc;
    }

    /* Cursor implementation */
    int WiredTigerIndex::IndexCursor::getDirection() const { return _forward ? 1 : -1; }

    bool WiredTigerIndex::IndexCursor::isEOF() const { return _eof; }

    bool WiredTigerIndex::IndexCursor::pointsToSamePlaceAs(const SortedDataInterface::Cursor &genother) const
    {
	    const WiredTigerIndex::IndexCursor &other = dynamic_cast<const WiredTigerIndex::IndexCursor &>(genother);
	    WT_CURSOR *c = _cursor.Get(), *otherc = other._cursor.Get();
	    int cmp, ret = c->compare(c, otherc, &cmp);
	    invariant(ret == 0);
	    return cmp == 0;
    }

    void WiredTigerIndex::IndexCursor::aboutToDeleteBucket(const DiskLoc& bucket) {
	invariant(!"aboutToDeleteBucket should not be called");
    }

    bool WiredTigerIndex::IndexCursor::locate(const BSONObj &key, const DiskLoc& loc) {
	WT_CURSOR *c = _cursor.Get();
	int cmp = -1, ret;
	if (key == BSONObj()) {
	    ret = c->reset(c);
	    advance();
	    return !isEOF();
	}

	c->set_key(c, _toItem(key).Get(), _toItem(loc).Get());
	ret = c->search_near(c, &cmp);
	// Make sure we land on a matching key
	if (ret == 0 && cmp < 0)
	    ret = c->next(c);
	if (ret == 0) {
	    WT_ITEM keyItem, locItem;
	    ret = c->get_key(c, &keyItem, &locItem);
	    invariant(ret == 0);
	    if (key != BSONObj(static_cast<const char *>(keyItem.data)))
		ret = WT_NOTFOUND;
	}
	if (ret == WT_NOTFOUND) {
	    _eof = true;
	    return false;
	}
	invariant(ret == 0);
	_eof = false;
	return true;
    }

    void WiredTigerIndex::IndexCursor::customLocate(const BSONObj& keyBegin,
			      int keyBeginLen,
			      bool afterKey,
			      const vector<const BSONElement*>& keyEnd,
			      const vector<bool>& keyEndInclusive) {
	// TODO
	invariant(0);
    }

    void WiredTigerIndex::IndexCursor::advanceTo(const BSONObj &keyBegin,
		   int keyBeginLen,
		   bool afterKey,
		   const vector<const BSONElement*>& keyEnd,
		   const vector<bool>& keyEndInclusive) {
	// XXX I think these do the same thing????
	customLocate(keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive);
    }

    BSONObj WiredTigerIndex::IndexCursor::getKey() const {
	WT_CURSOR *c = _cursor.Get();
	WT_ITEM keyItem, locItem;
	int ret = c->get_key(c, &keyItem, &locItem);
	invariant(ret == 0);
	return BSONObj(static_cast<const char *>(keyItem.data));
    }

    DiskLoc WiredTigerIndex::IndexCursor::getDiskLoc() const {
	WT_CURSOR *c = _cursor.Get();
	WT_ITEM keyItem, locItem;
	int ret = c->get_key(c, &keyItem, &locItem);
	invariant(ret == 0);
	return reinterpret_cast<const DiskLoc *>(locItem.data)[0];
    }

    void WiredTigerIndex::IndexCursor::advance() {
	WT_CURSOR *c = _cursor.Get();
	int ret = c->next(c);
	if (ret == WT_NOTFOUND)
	    _eof = true;
	else {
	    invariant(ret == 0);
	    _eof = false;
	}
    }

    void WiredTigerIndex::IndexCursor::savePosition() {
	if (isEOF())
	    _savedAtEnd = true;
	else {
	    _savedKey = getKey();
	    _savedLoc = getDiskLoc();
	}
    }

    void WiredTigerIndex::IndexCursor::restorePosition() {
	if (_savedAtEnd)
	    _eof = true;
	else
	    locate(_savedKey, _savedLoc);
    }

    SortedDataInterface::Cursor* WiredTigerIndex::newCursor(OperationContext* txn, int direction) const {
	invariant((direction == 1) || (direction == -1));
	// XXX leak -- we need a session associated with the txn.
	WiredTigerSession *session = new WiredTigerSession(_db.GetSession(), _db);
	return new IndexCursor(GetCursor(*session, true), *session, txn, direction == 1);
    }

    Status WiredTigerIndex::initAsEmpty(OperationContext* txn) {
	// No-op
	return Status::OK();
    }

    WT_CURSOR *WiredTigerIndex::GetCursor(WiredTigerSession &session, bool acquire) const {
	    return session.GetCursor(GetURI(), acquire);
    }
    const std::string &WiredTigerIndex::GetURI() const { return _uri; }

    SortedDataInterface* getWiredTigerIndex(WiredTigerDatabase &db, const std::string &ns, const std::string &idxName, IndexCatalogEntry& info, boost::shared_ptr<void>* dataInOut) {
	int ret = WiredTigerIndex::Create(db, ns, idxName, info);
	invariant(ret == 0);
	return new WiredTigerIndex(db, info, ns, idxName);
    }
    
    class WiredTigerBuilderImpl : public SortedDataBuilderInterface {
    public:
        WiredTigerBuilderImpl(WiredTigerIndex &idx, OperationContext *txn, bool dupsAllowed)
                : _idx(idx), _txn(txn), _dupsAllowed(dupsAllowed), _count(0) { }

        ~WiredTigerBuilderImpl() { }

        Status addKey(const BSONObj& key, const DiskLoc& loc) {
	    Status s = _idx.insert(_txn, key, loc, _dupsAllowed);
	    if (s.isOK())
		    _count++;
            return s;
        }

        unsigned long long commit(bool mayInterrupt) {
            return _count;
        }

    private:
	WiredTigerIndex &_idx;
	OperationContext *_txn;
	bool _dupsAllowed;
        unsigned long long _count;
    };

    SortedDataBuilderInterface* WiredTigerIndex::getBulkBuilder(OperationContext* txn, bool dupsAllowed) {
	return new WiredTigerBuilderImpl(*this, txn, dupsAllowed);
    }

}  // namespace mongo
