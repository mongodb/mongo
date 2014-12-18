// wiredtiger_index.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"

#include <set>

#include "mongo/db/json.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {
    static const int TempKeyMaxSize = 1024; // this goes away with SERVER-3372

    static const WiredTigerItem emptyItem(NULL, 0);

    static const int kMinimumIndexVersion = 1;
    static const int kCurrentIndexVersion = 1; // New indexes use this by default.
    static const int kMaximumIndexVersion = 1;
    BOOST_STATIC_ASSERT(kCurrentIndexVersion >= kMinimumIndexVersion);
    BOOST_STATIC_ASSERT(kCurrentIndexVersion <= kMaximumIndexVersion);

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

    // How RecordIds are stored on disk.
    typedef int64_t RecordIdRepr;

    RecordId getRecordIdAt(const void* ptr, size_t byteOffset = 0) {
        RecordIdRepr repr;
        memcpy(&repr, static_cast<const char*>(ptr) + byteOffset, sizeof(repr));
        return RecordId(repr);
    }

    void putRecordId(void* dest, RecordId loc) {
        const RecordIdRepr repr = loc.repr();
        memcpy(dest, &repr, sizeof(repr));
    }

    void putRecordIdAtOffset(void* dest, size_t byteOffset, RecordId loc) {
        putRecordId(static_cast<char*>(dest) + byteOffset, loc);
    }

    /**
     * Constructs an IndexKeyEntry from a slice containing the bytes of a BSONObject followed
     * by the bytes of a RecordId
     */
    static IndexKeyEntry makeIndexKeyEntry(const WT_ITEM *keyCols) {
        const char* data = static_cast<const char*>( keyCols->data );
        BSONObj key( data );
        if ( keyCols->size == static_cast<size_t>( key.objsize() ) ) {
            // in unique mode
            return IndexKeyEntry( key, RecordId() );
        }
        invariant( keyCols->size == key.objsize() + sizeof(RecordIdRepr) );

        return IndexKeyEntry( key, getRecordIdAt(data + key.objsize()));
    }

    WiredTigerItem _toItem( const BSONObj& key, const RecordId& loc,
                            boost::scoped_array<char>*out ) {
        size_t keyLen = key.objsize() + sizeof(RecordIdRepr);
        out->reset( new char[keyLen] );
        memcpy( out->get(), key.objdata(), key.objsize() );
        putRecordIdAtOffset(out->get(), key.objsize(), loc);

        return WiredTigerItem( out->get(), keyLen );
    }

    /**
     * Custom comparator used to compare Index Entries by BSONObj and RecordId
     */
    struct WiredTigerIndexCollator : public WT_COLLATOR {
        public:
            WiredTigerIndexCollator(const Ordering& order)
                    :WT_COLLATOR(), _indexComparator(order) {
                compare = _compare;
                terminate = _terminate;
            }

            int Compare(WT_SESSION *s, const WT_ITEM *a, const WT_ITEM *b) const {
                const IndexKeyEntry lhs = makeIndexKeyEntry(a);
                const IndexKeyEntry rhs = makeIndexKeyEntry(b);
                int cmp = _indexComparator.compare( lhs, rhs );
                if (cmp < 0)
                    cmp = -1;
                else if (cmp > 0)
                    cmp = 1;
                return cmp;
            }

            static int _compare(WT_COLLATOR *coll,
                                WT_SESSION *s,
                                const WT_ITEM *a,
                                const WT_ITEM *b,
                                int *cmp) {

                try {
                    WiredTigerIndexCollator *c = static_cast<WiredTigerIndexCollator *>(coll);
                    *cmp = c->Compare(s, a, b);
                    return 0;
                }
                catch (...) {
                    std::terminate();
                }
            }

            static int _terminate(WT_COLLATOR *coll, WT_SESSION *s) {
                try {
                    WiredTigerIndexCollator *c = static_cast<WiredTigerIndexCollator *>(coll);
                    delete c;
                    return 0;
                }
                catch (...) {
                    std::terminate();
                }
            }

        private:
            const IndexEntryComparison _indexComparator;
    };

    int index_collator_customize(WT_COLLATOR *coll,
                                 WT_SESSION *s,
                                 const char *uri,
                                 WT_CONFIG_ITEM *metadata,
                                 WT_COLLATOR **collp) {
        try {
            invariant(metadata->type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);
            WiredTigerConfigParser config(*metadata);

            WT_CONFIG_ITEM version;
            if (config.get("formatVersion", &version) != 0) {
                fassertFailedWithStatusNoTrace(28580,
                    Status(ErrorCodes::UnsupportedFormat, str::stream()
                           << "Found an index from an unsupported RC version."
                           << " Please restart with --repair to fix."));

            }

            WT_CONFIG_ITEM infoObj;
            const int infoObjRet = config.get("infoObj", &infoObj);
            if (infoObjRet != 0) {
                // This must be from a future version from when we removed infoObj. Print something
                // reasonable.
                infoObj.str = "(unknown)";
                infoObj.len = strlen(infoObj.str);
            }

            if (version.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM
                    || version.val < kMinimumIndexVersion || version.val > kMaximumIndexVersion) {
                fassertFailedWithStatusNoTrace(28579,
                    Status(ErrorCodes::UnsupportedFormat, str::stream()
                           << "Index " << infoObj.str
                           << " has unsupported formatVersion " << version.str
                           << ". Please restart with --repair to fix."));
            }

            invariant(infoObjRet == 0);

            IndexDescriptor desc(0, "unknown", fromjson(std::string(infoObj.str, infoObj.len)));
            *collp = new WiredTigerIndexCollator(Ordering::make(desc.keyPattern()));
            return 0;
        }
        catch (...) {
            std::terminate();
        }
    }

    extern "C" MONGO_COMPILER_API_EXPORT int index_collator_extension(WT_CONNECTION *conn, WT_CONFIG_ARG *cfg) {
            static WT_COLLATOR idx_static;

            idx_static.customize = index_collator_customize;
            return conn->add_collator(conn, "mongo_index", &idx_static, NULL);
    }

    // taken from btree_logic.cpp
    Status dupKeyError(const BSONObj& key) {
        StringBuilder sb;
        sb << "E11000 duplicate key error ";
        sb << "dup key: " << key;
        return Status(ErrorCodes::DuplicateKey, sb.str());
    }

    Status checkKeySize(const BSONObj& key) {
        if ( key.objsize() >= TempKeyMaxSize ) {
            string msg = mongoutils::str::stream()
                << "WiredTigerIndex::insert: key too large to index, failing "
                << ' ' << key.objsize() << ' ' << key;
            return Status(ErrorCodes::KeyTooLong, msg);
        }
        return Status::OK();
    }

} // namespace

    // static
    StatusWith<std::string> WiredTigerIndex::parseIndexOptions(const BSONObj& options) {
        BSONForEach(elem, options) {
            if (elem.fieldNameStringData() == "configString") {
                if (elem.type() != String) {
                    return StatusWith<std::string>(ErrorCodes::TypeMismatch, str::stream()
                        << "configString must be a string. "
                        << "Not adding 'configString' value "
                        << elem << " to index configuration");
                }
                if (elem.valueStringData().empty()) {
                    return StatusWith<std::string>(ErrorCodes::InvalidOptions,
                        "configString must be not be an empty string.");
                }
                return StatusWith<std::string>(elem.String());
            }
            else {
                // Return error on first unrecognized field.
                return StatusWith<std::string>(ErrorCodes::InvalidOptions, str::stream()
                    << '\'' << elem.fieldNameStringData() << '\''
                    << " is not a supported option.");
            }
        }
        return StatusWith<std::string>(ErrorCodes::BadValue,
            "Storage engine options document must not be empty.");
    }

    // static
    StatusWith<std::string> WiredTigerIndex::generateCreateString(const std::string& extraConfig,
                                                                  const IndexDescriptor& desc) {
        str::stream ss;

        // Separate out a prefix and suffix in the default string. User configuration will
        // override values in the prefix, but not values in the suffix.
        ss << "type=file,leaf_page_max=16k,";
        if (wiredTigerGlobalOptions.useIndexPrefixCompression) {
            ss << "prefix_compression,";
        }

        // TODO: remove this; SERVER-16568
        std::string localIndexBlockCompressor;
        if (wiredTigerGlobalOptions.indexBlockCompressor == "none") {
            localIndexBlockCompressor = "";
        }
        else {
            localIndexBlockCompressor = wiredTigerGlobalOptions.indexBlockCompressor;
        }

        ss << "block_compressor=" << localIndexBlockCompressor << ",";
        ss << extraConfig;

        // Validate configuration object.
        // Raise an error about unrecognized fields that may be introduced in newer versions of
        // this storage engine.
        // Ensure that 'configString' field is a string. Raise an error if this is not the case.
        BSONElement storageEngineElement = desc.getInfoElement("storageEngine");
        if (storageEngineElement.isABSONObj()) {
            BSONObj storageEngine = storageEngineElement.Obj();
            StatusWith<std::string> parseStatus =
                parseIndexOptions(storageEngine.getObjectField(kWiredTigerEngineName));
            if (!parseStatus.isOK()) {
                return parseStatus;
            }
            if (!parseStatus.getValue().empty()) {
                ss << "," << parseStatus.getValue();
            }
        }

        // WARNING: No user-specified config can appear below this line. These options are required
        // for correct behavior of the server.

        // Indexes need to store the metadata for collation to work as expected.
        ss << ",key_format=u,value_format=u,collator=mongo_index";

        // Index metadata
        ss << ",app_metadata=("
                << "formatVersion=" << kCurrentIndexVersion << ','
                << "infoObj=" << desc.infoObj().jsonString()
            << "),";

        LOG(3) << "index create string: " << ss.ss.str();
        return StatusWith<std::string>(ss);
    }

    int WiredTigerIndex::Create(OperationContext* txn,
                                const std::string& uri,
                                const std::string& config) {
        WT_SESSION* s = WiredTigerRecoveryUnit::get( txn )->getSession()->getSession();
        LOG(1) << "create uri: " << uri << " config: " << config;
        return s->create(s, uri.c_str(), config.c_str());
    }

    WiredTigerIndex::WiredTigerIndex(const std::string& uri, const IndexDescriptor* desc)
        : _ordering(Ordering::make(desc->keyPattern())),
          _uri( uri ),
          _instanceId( WiredTigerSession::genCursorId() ) {
    }

    Status WiredTigerIndex::insert(OperationContext* txn,
              const BSONObj& key,
              const RecordId& loc,
              bool dupsAllowed) {
        invariant(loc.isNormal());
        dassert(!hasFieldNames(key));

        Status s = checkKeySize(key);
        if (!s.isOK())
            return s;

        WiredTigerCursor curwrap(_uri, _instanceId, txn);
        curwrap.assertInActiveTxn();
        WT_CURSOR *c = curwrap.get();

        return _insert( c, key, loc, dupsAllowed );
    }

    void WiredTigerIndex::unindex(OperationContext* txn,
                                  const BSONObj& key,
                                  const RecordId& loc,
                                  bool dupsAllowed ) {
        invariant(loc.isNormal());
        dassert(!hasFieldNames(key));

        WiredTigerCursor curwrap(_uri, _instanceId, txn);
        curwrap.assertInActiveTxn();
        WT_CURSOR *c = curwrap.get();
        invariant( c );

        _unindex( c, key, loc, dupsAllowed );
    }

    void WiredTigerIndex::fullValidate(OperationContext* txn, bool full, long long *numKeysOut,
                                       BSONObjBuilder* output) const {
        IndexCursor cursor(*this, txn, true );
        cursor.locate( minKey, RecordId::min() );
        long long count = 0;
        while ( !cursor.isEOF() ) {
            cursor.advance();
            count++;
        }
        if ( numKeysOut ) {
            *numKeysOut = count;
        }

        // Nothing further to do if 'full' validation is not requested.
        if (!full) {
            return;
        }

        invariant(output);

        {
            BSONObjBuilder metadata(output->subobjStart("metadata"));
            Status status = WiredTigerUtil::getApplicationMetadata(txn, uri(), &metadata);
            if (!status.isOK()) {
                metadata.append("error", "unable to retrieve metadata");
                metadata.append("code", static_cast<int>(status.code()));
                metadata.append("reason", status.reason());
            }
        }

        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession();
        WT_SESSION* s = session->getSession();
        Status status = WiredTigerUtil::exportTableToBSON(s, "statistics:" + uri(),
                                                          "statistics=(fast)", output);
        if (!status.isOK()) {
            output->append("error", "unable to retrieve statistics");
            output->append("code", static_cast<int>(status.code()));
            output->append("reason", status.reason());
        }
    }

    Status WiredTigerIndex::dupKeyCheck( OperationContext* txn,
                                         const BSONObj& key,
                                         const RecordId& loc) {
        invariant(!hasFieldNames(key));
        invariant(unique());

        WiredTigerCursor curwrap(_uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();

        if ( isDup(c, key, loc) )
            return dupKeyError(key);
        return Status::OK();
    }

    bool WiredTigerIndex::isEmpty(OperationContext* txn) {
        WiredTigerCursor curwrap(_uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        if (!c)
            return true;
        int ret = c->next(c);
        if (ret == WT_NOTFOUND)
            return true;
        invariantWTOK(ret);
        return false;
    }

    long long WiredTigerIndex::getSpaceUsedBytes( OperationContext* txn ) const {
        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession();
        return static_cast<long long>( WiredTigerUtil::getIdentSize( session->getSession(),
                                                                     _uri ) );
    }

    bool WiredTigerIndex::isDup(WT_CURSOR *c, const BSONObj& key, const RecordId& loc ) {
        invariant( unique() );
        // First check whether the key exists.
        WiredTigerItem item( key.objdata(), key.objsize() );
        c->set_key( c, item.Get() );
        int ret = c->search(c);
        if ( ret == WT_NOTFOUND )
            return false;
        invariantWTOK( ret );

        WT_ITEM value;
        invariantWTOK( c->get_value(c,&value) );
        for (size_t offset = 0; offset < value.size; offset += sizeof(RecordIdRepr)) {
            if (getRecordIdAt(value.data, offset) == loc)
                return false;
        }
        return true;
    }

    SortedDataInterface::Cursor* WiredTigerIndex::newCursor(OperationContext* txn,
                                                            int direction) const {
        invariant((direction == 1) || (direction == -1));
        return new IndexCursor(*this, txn, direction == 1);
    }

    Status WiredTigerIndex::initAsEmpty(OperationContext* txn) {
        // No-op
        return Status::OK();
    }

    /**
     * Base class for WiredTigerIndex bulk builders.
     *
     * Manages the bulk cursor used by bulk builders.
     */
    class WiredTigerIndex::BulkBuilder : public SortedDataBuilderInterface {
    public:
        BulkBuilder(WiredTigerIndex* idx, OperationContext* txn)
            : _ordering(idx->_ordering)
            , _txn(txn)
            , _session(WiredTigerRecoveryUnit::get(_txn)->getSessionCache()->getSession())
            , _cursor(openBulkCursor(idx))
        {}

        ~BulkBuilder() {
            _cursor->close(_cursor);
            WiredTigerRecoveryUnit::get(_txn)->getSessionCache()->releaseSession(_session);
        }

    protected:
        WT_CURSOR* openBulkCursor(WiredTigerIndex* idx) {
            // Open cursors can cause bulk open_cursor to fail with EBUSY.
            // TODO any other cases that could cause EBUSY?
            WiredTigerSession* outerSession = WiredTigerRecoveryUnit::get(_txn)->getSession();
            outerSession->closeAllCursors();

            // Not using cursor cache since we need to set "bulk".
            WT_CURSOR* cursor;
            // We use our own session to ensure we aren't in a transaction.
            WT_SESSION* session = _session->getSession();
            int err = session->open_cursor(session, idx->uri().c_str(), NULL, "bulk", &cursor);
            if (!err)
                return cursor;

            warning() << "failed to create WiredTiger bulk cursor: " << wiredtiger_strerror(err);
            warning() << "falling back to non-bulk cursor for index " << idx->uri();

            invariantWTOK(session->open_cursor(session, idx->uri().c_str(), NULL, NULL, &cursor));
            return cursor;
        }

        const Ordering _ordering;
        OperationContext* const _txn;
        WiredTigerSession* const _session;
        WT_CURSOR* const _cursor;
    };

    /**
     * Bulk builds a non-unique index.
     */
    class WiredTigerIndex::StandardBulkBuilder : public BulkBuilder {
    public:
        StandardBulkBuilder(WiredTigerIndex* idx, OperationContext* txn)
            : BulkBuilder(idx, txn) {
        }

        Status addKey(const BSONObj& key, const RecordId& loc) {
            {
                const Status s = checkKeySize(key);
                if (!s.isOK())
                    return s;
            }

            // Build a buffer with the key and loc concatenated.
            const size_t keyLen = key.objsize() + sizeof(RecordIdRepr);
            invariant(keyLen <= kBufferSize);
            memcpy(_buffer, key.objdata(), key.objsize());
            putRecordIdAtOffset(_buffer, key.objsize(), loc);

            // Can't use WiredTigerCursor since we aren't using the cache.
            WiredTigerItem item(_buffer, keyLen);
            _cursor->set_key(_cursor, item.Get() );
            _cursor->set_value(_cursor, &emptyItem);
            invariantWTOK(_cursor->insert(_cursor));
            invariantWTOK(_cursor->reset(_cursor));

            return Status::OK();
        }

        void commit(bool mayInterrupt) {
            // TODO do we still need this?
            // this is bizarre, but required as part of the contract
            WriteUnitOfWork uow( _txn );
            uow.commit();
        }

    private:
        // Will need to support dynamic sizing if we remove TempKeyMaxSize.
        static const size_t kBufferSize = TempKeyMaxSize + sizeof(RecordIdRepr);
        char _buffer[kBufferSize];
    };

    /**
     * Bulk builds a unique index.
     *
     * In order to support unique indexes in dupsAllowed mode this class only does an actual insert
     * after it sees a key after the one we are trying to insert. This allows us to gather up all
     * duplicate locs and insert them all together. This is necessary since bulk cursors can only
     * append data.
     */
    class WiredTigerIndex::UniqueBulkBuilder : public BulkBuilder {
    public:
        UniqueBulkBuilder(WiredTigerIndex* idx, OperationContext* txn, bool dupsAllowed)
            : BulkBuilder(idx, txn), _dupsAllowed(dupsAllowed) {
        }

        Status addKey(const BSONObj& newKey, const RecordId& loc) {
            {
                const Status s = checkKeySize(newKey);
                if (!s.isOK())
                    return s;
            }

            const int cmp = newKey.woCompare(_key, _ordering);
            if (cmp != 0) {
                if (!_key.isEmpty()) { // _key.isEmpty() is only true on the first call to addKey().
                    invariant(cmp > 0); // newKey must be > the last key
                    // We are done with dups of the last key so we can insert it now.
                    doInsert();
                }
                invariant(_locs.empty());
            }
            else {
                // Dup found!
                if (!_dupsAllowed) {
                    return dupKeyError(newKey);
                }

                // If we get here, we are in the weird mode where dups are allowed on a unique
                // index, so add ourselves to the list of duplicate locs. This also replaces the
                // _key which is correct since any dups seen later are likely to be newer.
            }

            _key = newKey.getOwned();
            _locs.push_back(loc.repr());

            return Status::OK();
        }

        void commit(bool mayInterrupt) {
            WriteUnitOfWork uow( _txn );
            if (!_locs.empty()) {
                // This handles inserting the last unique key.
                doInsert();
            }
            uow.commit();
        }

    private:
        void doInsert() {

            invariant(!_locs.empty());

            WiredTigerItem keyItem(_key.objdata(), _key.objsize());
            _cursor->set_key(_cursor, keyItem.Get());

            invariant(_locs.size() > 0);
            WiredTigerItem valueItem(&_locs.front(), _locs.size() * sizeof(RecordIdRepr));
            _cursor->set_value(_cursor, valueItem.Get());

            invariantWTOK(_cursor->insert(_cursor));
            invariantWTOK(_cursor->reset(_cursor));

            _locs.clear();
        }

        const bool _dupsAllowed;
        BSONObj _key;
        std::vector<RecordIdRepr> _locs;
    };

    SortedDataBuilderInterface* WiredTigerIndex::getBulkBuilder( OperationContext* txn,
                                                                 bool dupsAllowed ) {
        if ( !dupsAllowed ) {
            // if we don't allow dups, we better be unique
            invariant( unique() );
        }

        if (unique()) {
            return new UniqueBulkBuilder(this, txn, dupsAllowed);
        }
        else {
            return new StandardBulkBuilder(this, txn);
        }
    }



    // ----------------------

    WiredTigerIndex::IndexCursor::IndexCursor(const WiredTigerIndex &idx,
            OperationContext *txn,
            bool forward)
       : _txn(txn),
         _cursor(idx.uri(), idx.instanceId(), txn ),
         _idx(idx),
         _forward(forward),
         _eof(true),
         _uniqueLen( -1 ) {
    }

    bool WiredTigerIndex::IndexCursor::pointsToSamePlaceAs( const SortedDataInterface::Cursor &genother) const {
        const WiredTigerIndex::IndexCursor &other =
            dynamic_cast<const WiredTigerIndex::IndexCursor &>(genother);

        if ( _eof && other._eof )
            return true;
        else if ( _eof || other._eof )
            return false;

        if ( getRecordId() != other.getRecordId() )
            return false;

        return getKey() == other.getKey();
    }

    void WiredTigerIndex::IndexCursor::aboutToDeleteBucket(const RecordId& bucket) {
        invariant(!"aboutToDeleteBucket should not be called");
    }

    bool WiredTigerIndex::IndexCursor::_locate(const BSONObj &key, const RecordId& loc) {
        _uniqueLen = -1;
        WT_CURSOR *c = _cursor.get();

        RecordId searchLoc = loc;
        // Null cursors should start at the zero key to maintain search ordering in the
        // collator.
        // Reverse cursors should start on the last matching key.
        if (loc.isNull())
            searchLoc = _forward ? RecordId::min() : RecordId::max();

        boost::scoped_array<char> data;
        WiredTigerItem myKey = _toItem( key, searchLoc, &data );

        int cmp = -1;
        c->set_key(c, myKey.Get() );


        int ret = c->search_near(c, &cmp);
        if ( ret == WT_NOTFOUND ) {
            _eof = true;
            return false;
        }
        invariantWTOK( ret );
        // Make sure we land on a matching key
        if ( _forward ? cmp < 0 : cmp > 0 )
            ret = _forward ? c->next(c) : c->prev(c);

        _eof = ret != 0;

        if ( _eof ) {
            return false;
        }

        if ( key != getKey() ) {
            return false;
        }

        if ( !_idx.unique() ) {
            return true;
        }

        // now we need to check if we have an array situation

        if ( loc.isNull() ) {
            // no loc specified means start and beginning or end of array as needed
            // so nothing to do
            return true;
        }

        // we're looking for a specific RecordId, lets see if we can find

        WT_ITEM item;
        invariantWTOK( c->get_value(c, &item ) );
        _uniqueLen = item.size / sizeof(RecordIdRepr);
        invariant( _uniqueLen > 0 );

        if ( _forward ) {
            _uniquePos = 0;
            for ( ; _uniquePos < _uniqueLen; _uniquePos++ ) {
                const RecordId temp = getRecordIdAt(item.data, _uniquePos * sizeof(RecordIdRepr));
                if ( temp == loc )
                    break;

                if ( loc < temp )
                    break;
            }
        }
        else {
            _uniquePos = _uniqueLen-1;
            for ( ; _uniquePos >= 0; _uniquePos-- ) {
                const RecordId temp = getRecordIdAt(item.data, _uniquePos * sizeof(RecordIdRepr));
                if ( temp == loc )
                    break;

                if ( temp < loc )
                    break;
            }
            _uniquePos = _uniqueLen - 1 - _uniquePos;
        }

        if ( _uniquePos == _uniqueLen ) {
            // we need to move to next slot
            advance();
        }

        return true;
    }

    bool WiredTigerIndex::IndexCursor::locate(const BSONObj &key, const RecordId& loc) {
        const BSONObj finalKey = stripFieldNames(key);
        bool result = _locate(finalKey, loc);

        // An explicit search at the start of the range should always return false
        if (loc == RecordId::min() || loc == RecordId::max() )
            return false;
        return result;
   }

    void WiredTigerIndex::IndexCursor::advanceTo(const BSONObj &keyBegin,
           int keyBeginLen,
           bool afterKey,
           const vector<const BSONElement*>& keyEnd,
           const vector<bool>& keyEndInclusive) {

        BSONObj key = IndexEntryComparison::makeQueryObject(
                         keyBegin, keyBeginLen,
                         afterKey, keyEnd, keyEndInclusive, getDirection() );

        _locate(key, RecordId());
    }

    void WiredTigerIndex::IndexCursor::customLocate(const BSONObj& keyBegin,
                  int keyBeginLen,
                  bool afterKey,
                  const vector<const BSONElement*>& keyEnd,
                  const vector<bool>& keyEndInclusive) {
        advanceTo(keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive);
    }

    BSONObj WiredTigerIndex::IndexCursor::getKey() const {
        WT_CURSOR *c = _cursor.get();
        WT_ITEM keyItem;
        int ret = c->get_key(c, &keyItem);
        invariantWTOK(ret);
        return makeIndexKeyEntry(&keyItem).key;
    }

    RecordId WiredTigerIndex::IndexCursor::getRecordId() const {
        if ( _eof )
            return RecordId();

        WT_CURSOR *c = _cursor.get();
        WT_ITEM item;
        if ( _idx.unique() ) {
            invariantWTOK( c->get_value(c, &item ) );
            if ( _uniqueLen == -1 ) {
                // first time at this spot
                _uniqueLen = item.size / sizeof(RecordIdRepr);
                invariant( _uniqueLen > 0 );
                _uniquePos = 0;
            }

            RecordId loc;
            int posToUse = _uniquePos;
            if ( !_forward )
                posToUse = _uniqueLen - 1 - _uniquePos;

            loc = getRecordIdAt(item.data, posToUse * sizeof(RecordIdRepr));
            invariant( posToUse >= 0 && posToUse < _uniqueLen );

            return loc;
        }
        invariantWTOK( c->get_key(c, &item) );
        return makeIndexKeyEntry( &item ).loc;
    }

    void WiredTigerIndex::IndexCursor::advance() {
        // Advance on a cursor at the end is a no-op
        if ( _eof )
            return;

        if ( _idx.unique() ) {
            if ( _uniqueLen == -1 ) {
                // we need to investigate
                getRecordId();
            }

            _uniquePos++; // advance

            if ( _uniquePos < _uniqueLen ) {
                return;
            }

        }

        _uniqueLen = -1;


        WT_CURSOR *c = _cursor.get();
        int ret = _forward ? c->next(c) : c->prev(c);
        if ( ret == WT_NOTFOUND ) {
            _eof = true;
            return;
        }
        invariantWTOK(ret);
        _eof = false;
    }

    void WiredTigerIndex::IndexCursor::savePosition() {
        _savedForCheck = _txn->recoveryUnit();

        if ( !wt_keeptxnopen() && !_eof ) {
            _savedKey = getKey().getOwned();
            _savedLoc = getRecordId();
            _cursor.reset();
        }

        _txn = NULL;
    }

    void WiredTigerIndex::IndexCursor::restorePosition( OperationContext *txn ) {
        // Update the session handle with our new operation context.
        _txn = txn;
        invariant( _savedForCheck == txn->recoveryUnit() );

        if ( !wt_keeptxnopen() && !_eof ) {
            _locate(_savedKey, _savedLoc);
        }
    }

    // ------------------------------

    WiredTigerIndexUnique::WiredTigerIndexUnique( const std::string& uri,
                                                  const IndexDescriptor* desc )
        : WiredTigerIndex( uri, desc ) {
    }

    Status WiredTigerIndexUnique::_insert( WT_CURSOR* c,
                                           const BSONObj& key,
                                           const RecordId& loc,
                                           bool dupsAllowed ) {

        WiredTigerItem keyItem( key.objdata(), key.objsize() );
        RecordIdRepr locRepr = loc.repr();
        WiredTigerItem valueItem( &locRepr, sizeof(locRepr) );
        c->set_key( c, keyItem.Get() );
        c->set_value( c, valueItem.Get() );
        int ret = c->insert( c );

        if ( ret == WT_ROLLBACK && !dupsAllowed ) {
            // if there is a conflict on a unique key, it means there is a dup key
            // even if someone else is deleting at the same time, its ok to fail this
            // insert as a dup key as it a race
            return dupKeyError(key);
        }
        else if ( ret != WT_DUPLICATE_KEY ) {
            return wtRCToStatus( ret );
        }

        // we might be in weird mode where there might be multiple values
        // we put them all in the "list"
        ret = c->search(c);
        invariantWTOK( ret );

        WT_ITEM old;
        invariantWTOK( c->get_value(c, &old ) );

        // Optimizing common case: there is only one loc currently in the index.
        if (old.size == sizeof(RecordIdRepr)) {
            if (loc == getRecordIdAt(old.data)) {
                return Status::OK(); // already in index
            }
            else if (!dupsAllowed) {
                return dupKeyError(key);
            }
        }

        // Fall back to slow case where there may be multiple locs.

        std::set<RecordId> all;

        // see if its already in the array
        for ( size_t i = 0; i < (old.size/sizeof(RecordIdRepr)); i++ ) {
            const RecordId temp = getRecordIdAt(old.data, i * sizeof(RecordIdRepr));
            if ( loc == temp )
                return Status::OK();
            all.insert( RecordId(temp) );
        }

        if ( !dupsAllowed ) {
            return dupKeyError(key);
        }

        all.insert( loc );

        // not in the array, add it to the back
        size_t newSize = all.size() * sizeof(RecordIdRepr);
        boost::scoped_array<char> bigger( new char[newSize] );

        size_t offset = 0;
        for ( std::set<RecordId>::const_iterator it = all.begin(); it != all.end(); ++it ) {
            putRecordIdAtOffset(bigger.get(), offset, *it);
            offset += sizeof(RecordIdRepr);
        }

        valueItem = WiredTigerItem( bigger.get(), newSize );
        c->set_value( c, valueItem.Get() );
        return wtRCToStatus( c->update( c ) );
    }

    void WiredTigerIndexUnique::_unindex( WT_CURSOR* c,
                                          const BSONObj& key,
                                          const RecordId& loc,
                                          bool dupsAllowed ) {
        WiredTigerItem keyItem( key.objdata(), key.objsize() );
        c->set_key( c, keyItem.Get() );

        if ( !dupsAllowed ) {
            // nice and clear
            int ret = c->remove(c);
            if (ret == WT_NOTFOUND) {
                return;
            }
            invariantWTOK(ret);
            return;
        }

        // dups are allowed, so we have to deal with a vector of RecordId

        int ret = c->search(c);
        if ( ret == WT_NOTFOUND )
            return;
        invariantWTOK( ret );

        WT_ITEM old;
        invariantWTOK( c->get_value(c, &old ) );

        // see if its in the array
        size_t num = old.size / sizeof(RecordIdRepr);
        for ( size_t i = 0; i < num; i++ ) {
            const RecordId temp = getRecordIdAt(old.data, i * sizeof(RecordIdRepr));
            if ( loc != temp )
                continue;

            // we found it, now lets re-save array without it
            size_t newSize = old.size - sizeof(RecordIdRepr);

            if ( newSize == 0 ) {
                // nothing left, just delete entry
                invariantWTOK( c->remove(c) );
                return;
            }

            boost::scoped_array<char> smaller( new char[newSize] );
            size_t offset = i * sizeof(RecordIdRepr);
            memcpy( smaller.get(), old.data, offset );
            memcpy( smaller.get() + offset,
                    static_cast<const char*>( old.data ) + offset + sizeof(RecordIdRepr),
                    old.size - sizeof(RecordIdRepr) - offset );
            WiredTigerItem valueItem = WiredTigerItem( smaller.get(), newSize );
            c->set_value( c, valueItem.Get() );
            invariantWTOK( c->update( c ) );
        }
    }

    // ------------------------------

    WiredTigerIndexStandard::WiredTigerIndexStandard( const std::string& uri,
                                                      const IndexDescriptor* desc )
        : WiredTigerIndex( uri, desc ) {
    }

    Status WiredTigerIndexStandard::_insert( WT_CURSOR* c,
                                             const BSONObj& key,
                                             const RecordId& loc,
                                             bool dupsAllowed ) {
        invariant( dupsAllowed );

        boost::scoped_array<char> data;
        WiredTigerItem item = _toItem( key, loc, &data );
        c->set_key(c, item.Get() );
        c->set_value(c, &emptyItem);
        int ret = c->insert( c );

        if ( ret != WT_DUPLICATE_KEY )
            return wtRCToStatus( ret );
        // If the record was already in the index, we just return OK.
        // This can happen, for example, when building a background index while documents are being
        // written and reindexed.
        return Status::OK();
    }

    void WiredTigerIndexStandard::_unindex( WT_CURSOR* c,
                                            const BSONObj& key,
                                            const RecordId& loc,
                                            bool dupsAllowed ) {
        invariant( dupsAllowed );
        boost::scoped_array<char> data;
        WiredTigerItem item = _toItem( key, loc, &data);
        c->set_key(c, item.Get() );
        int ret = c->remove(c);
        if (ret != WT_NOTFOUND) {
            invariantWTOK(ret);
        }
    }


}  // namespace mongo
