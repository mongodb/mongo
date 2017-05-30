/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection_info_cache.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
class CollectionCatalogEntry;
class DatabaseCatalogEntry;
class ExtentManager;
class IndexCatalog;
class DatabaseImpl;
class MatchExpression;
class MultiIndexBlock;
class OpDebug;
class OperationContext;
struct OplogUpdateEntryArgs;
class RecordCursor;
class RecordFetcher;
class UpdateDriver;
class UpdateRequest;

struct CompactOptions {
    // padding
    enum PaddingMode { PRESERVE, NONE, MANUAL } paddingMode = NONE;

    // only used if _paddingMode == MANUAL
    double paddingFactor = 1;  // what to multiple document size by
    int paddingBytes = 0;      // what to add to ducment size after multiplication

    // other
    bool validateDocuments = true;

    std::string toString() const;

    unsigned computeRecordSize(unsigned recordSize) const {
        recordSize = static_cast<unsigned>(paddingFactor * recordSize);
        recordSize += paddingBytes;
        return recordSize;
    }
};

struct CompactStats {
    long long corruptDocuments = 0;
};

struct InsertStatement {
public:
    InsertStatement() = default;
    explicit InsertStatement(BSONObj toInsert) : doc(toInsert) {}

    InsertStatement(StmtId statementId, BSONObj toInsert) : stmtId(statementId), doc(toInsert) {}

    StmtId stmtId = kUninitializedStmtId;
    BSONObj doc;
};

/**
 * Queries with the awaitData option use this notifier object to wait for more data to be
 * inserted into the capped collection.
 */
class CappedInsertNotifier {
public:
    CappedInsertNotifier();

    /**
     * Wakes up all threads waiting.
     */
    void notifyAll();

    /**
     * Waits for 'timeout' microseconds, or until notifyAll() is called to indicate that new
     * data is available in the capped collection.
     *
     * NOTE: Waiting threads can be signaled by calling kill or notify* methods.
     */
    void wait(Microseconds timeout) const;

    /**
     * Same as above but also ensures that if the version has changed, it also returns.
     */
    void wait(uint64_t prevVersion, Microseconds timeout) const;

    /**
     * Returns the version for use as an additional wake condition when used above.
     */
    uint64_t getVersion() const {
        return _version;
    }

    /**
     * Same as above but without a timeout.
     */
    void wait() const;

    /**
     * Cancels the notifier if the collection is dropped/invalidated, and wakes all waiting.
     */
    void kill();

    /**
     * Returns true if no new insert notification will occur.
     */
    bool isDead();

private:
    // Helper for wait impls.
    void _wait(stdx::unique_lock<stdx::mutex>& lk,
               uint64_t prevVersion,
               Microseconds timeout) const;

    // Signalled when a successful insert is made into a capped collection.
    mutable stdx::condition_variable _notifier;

    // Mutex used with '_notifier'. Protects access to '_version'.
    mutable stdx::mutex _mutex;

    // A counter, incremented on insertion of new data into the capped collection.
    //
    // The condition which '_cappedNewDataNotifier' is being notified of is an increment of this
    // counter. Access to this counter is synchronized with '_mutex'.
    uint64_t _version;

    // True once the notifier is dead.
    bool _dead;
};

/**
 * this is NOT safe through a yield right now.
 * not sure if it will be, or what yet.
 */
class Collection final : CappedCallback, UpdateNotifier {
public:
    enum ValidationAction { WARN, ERROR_V };
    enum ValidationLevel { OFF, MODERATE, STRICT_V };

    class Impl : virtual CappedCallback, virtual UpdateNotifier {
    public:
        virtual ~Impl() = 0;

        virtual void init(OperationContext* opCtx) = 0;

    private:
        friend Collection;
        virtual DatabaseCatalogEntry* dbce() const = 0;

        virtual CollectionCatalogEntry* details() const = 0;

        virtual Status aboutToDeleteCapped(OperationContext* opCtx,
                                           const RecordId& loc,
                                           RecordData data) = 0;

        virtual Status recordStoreGoingToUpdateInPlace(OperationContext* opCtx,
                                                       const RecordId& loc) = 0;

    public:
        virtual bool ok() const = 0;

        virtual CollectionCatalogEntry* getCatalogEntry() = 0;
        virtual const CollectionCatalogEntry* getCatalogEntry() const = 0;

        virtual CollectionInfoCache* infoCache() = 0;
        virtual const CollectionInfoCache* infoCache() const = 0;

        virtual const NamespaceString& ns() const = 0;
        virtual OptionalCollectionUUID uuid() const = 0;

        virtual void refreshUUID(OperationContext* opCtx) = 0;

        virtual const IndexCatalog* getIndexCatalog() const = 0;
        virtual IndexCatalog* getIndexCatalog() = 0;

        virtual const RecordStore* getRecordStore() const = 0;
        virtual RecordStore* getRecordStore() = 0;

        virtual CursorManager* getCursorManager() const = 0;

        virtual bool requiresIdIndex() const = 0;

        virtual Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const = 0;

        virtual bool findDoc(OperationContext* opCtx,
                             const RecordId& loc,
                             Snapshotted<BSONObj>* out) const = 0;

        virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                                bool forward) const = 0;

        virtual std::vector<std::unique_ptr<RecordCursor>> getManyCursors(
            OperationContext* opCtx) const = 0;

        virtual void deleteDocument(OperationContext* opCtx,
                                    StmtId stmtId,
                                    const RecordId& loc,
                                    OpDebug* opDebug,
                                    bool fromMigrate,
                                    bool noWarn) = 0;

        virtual Status insertDocuments(OperationContext* opCtx,
                                       std::vector<InsertStatement>::const_iterator begin,
                                       std::vector<InsertStatement>::const_iterator end,
                                       OpDebug* opDebug,
                                       bool enforceQuota,
                                       bool fromMigrate) = 0;

        virtual Status insertDocument(OperationContext* opCtx,
                                      const InsertStatement& doc,
                                      OpDebug* opDebug,
                                      bool enforceQuota,
                                      bool fromMigrate) = 0;

        virtual Status insertDocumentsForOplog(OperationContext* opCtx,
                                               const DocWriter* const* docs,
                                               size_t nDocs) = 0;

        virtual Status insertDocument(OperationContext* opCtx,
                                      const BSONObj& doc,
                                      const std::vector<MultiIndexBlock*>& indexBlocks,
                                      bool enforceQuota) = 0;

        virtual StatusWith<RecordId> updateDocument(OperationContext* opCtx,
                                                    const RecordId& oldLocation,
                                                    const Snapshotted<BSONObj>& oldDoc,
                                                    const BSONObj& newDoc,
                                                    bool enforceQuota,
                                                    bool indexesAffected,
                                                    OpDebug* opDebug,
                                                    OplogUpdateEntryArgs* args) = 0;

        virtual bool updateWithDamagesSupported() const = 0;

        virtual StatusWith<RecordData> updateDocumentWithDamages(
            OperationContext* opCtx,
            const RecordId& loc,
            const Snapshotted<RecordData>& oldRec,
            const char* damageSource,
            const mutablebson::DamageVector& damages,
            OplogUpdateEntryArgs* args) = 0;

        virtual StatusWith<CompactStats> compact(OperationContext* opCtx,
                                                 const CompactOptions* options) = 0;

        virtual Status truncate(OperationContext* opCtx) = 0;

        virtual Status validate(OperationContext* opCtx,
                                ValidateCmdLevel level,
                                ValidateResults* results,
                                BSONObjBuilder* output) = 0;

        virtual Status touch(OperationContext* opCtx,
                             bool touchData,
                             bool touchIndexes,
                             BSONObjBuilder* output) const = 0;

        virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) = 0;

        virtual StatusWithMatchExpression parseValidator(const BSONObj& validator) const = 0;

        virtual Status setValidator(OperationContext* opCtx, BSONObj validator) = 0;

        virtual Status setValidationLevel(OperationContext* opCtx, StringData newLevel) = 0;
        virtual Status setValidationAction(OperationContext* opCtx, StringData newAction) = 0;

        virtual StringData getValidationLevel() const = 0;
        virtual StringData getValidationAction() const = 0;

        virtual bool isCapped() const = 0;

        virtual std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const = 0;

        virtual uint64_t numRecords(OperationContext* opCtx) const = 0;

        virtual uint64_t dataSize(OperationContext* opCtx) const = 0;

        virtual uint64_t getIndexSize(OperationContext* opCtx,
                                      BSONObjBuilder* details,
                                      int scale) = 0;

        virtual boost::optional<SnapshotName> getMinimumVisibleSnapshot() = 0;

        virtual void setMinimumVisibleSnapshot(SnapshotName name) = 0;

        virtual void notifyCappedWaitersIfNeeded() = 0;

        virtual const CollatorInterface* getDefaultCollator() const = 0;
    };

private:
    static std::unique_ptr<Impl> makeImpl(Collection* _this,
                                          OperationContext* opCtx,
                                          StringData fullNS,
                                          OptionalCollectionUUID uuid,
                                          CollectionCatalogEntry* details,
                                          RecordStore* recordStore,
                                          DatabaseCatalogEntry* dbce);

public:
    using factory_function_type = decltype(makeImpl);

    static void registerFactory(stdx::function<factory_function_type> factory);

    explicit inline Collection(OperationContext* const opCtx,
                               const StringData fullNS,
                               OptionalCollectionUUID uuid,
                               CollectionCatalogEntry* const details,  // does not own
                               RecordStore* const recordStore,         // does not own
                               DatabaseCatalogEntry* const dbce)       // does not own
        : _pimpl(makeImpl(this, opCtx, fullNS, uuid, details, recordStore, dbce)) {
        this->_impl().init(opCtx);
    }

    // Use this constructor only for testing/mocks
    explicit inline Collection(std::unique_ptr<Impl> mock) : _pimpl(std::move(mock)) {}

    inline ~Collection() = default;

    inline bool ok() const {
        return this->_impl().ok();
    }

    inline CollectionCatalogEntry* getCatalogEntry() {
        return this->_impl().getCatalogEntry();
    }

    inline const CollectionCatalogEntry* getCatalogEntry() const {
        return this->_impl().getCatalogEntry();
    }

    inline CollectionInfoCache* infoCache() {
        return this->_impl().infoCache();
    }

    inline const CollectionInfoCache* infoCache() const {
        return this->_impl().infoCache();
    }

    inline const NamespaceString& ns() const {
        return this->_impl().ns();
    }

    inline OptionalCollectionUUID uuid() const {
        return this->_impl().uuid();
    }

    inline void refreshUUID(OperationContext* opCtx) {
        return this->_impl().refreshUUID(opCtx);
    }

    inline const IndexCatalog* getIndexCatalog() const {
        return this->_impl().getIndexCatalog();
    }
    inline IndexCatalog* getIndexCatalog() {
        return this->_impl().getIndexCatalog();
    }

    inline const RecordStore* getRecordStore() const {
        return this->_impl().getRecordStore();
    }
    inline RecordStore* getRecordStore() {
        return this->_impl().getRecordStore();
    }

    inline CursorManager* getCursorManager() const {
        return this->_impl().getCursorManager();
    }

    inline bool requiresIdIndex() const {
        return this->_impl().requiresIdIndex();
    }

    inline Snapshotted<BSONObj> docFor(OperationContext* const opCtx, const RecordId& loc) const {
        return Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(),
                                    this->getRecordStore()->dataFor(opCtx, loc).releaseToBson());
    }

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    inline bool findDoc(OperationContext* const opCtx,
                        const RecordId& loc,
                        Snapshotted<BSONObj>* const out) const {
        return this->_impl().findDoc(opCtx, loc, out);
    }

    inline std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* const opCtx,
                                                           const bool forward = true) const {
        return this->_impl().getCursor(opCtx, forward);
    }

    /**
     * Returns many cursors that partition the Collection into many disjoint sets. Iterating
     * all returned cursors is equivalent to iterating the full collection.
     */
    inline std::vector<std::unique_ptr<RecordCursor>> getManyCursors(
        OperationContext* const opCtx) const {
        return this->_impl().getManyCursors(opCtx);
    }

    /**
     * Deletes the document with the given RecordId from the collection.
     *
     * 'fromMigrate' indicates whether the delete was induced by a chunk migration, and
     * so should be ignored by the user as an internal maintenance operation and not a
     * real delete.
     * 'loc' key to uniquely identify a record in a collection.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * 'cappedOK' if true, allows deletes on capped collections (Cloner::copyDB uses this).
     * 'noWarn' if unindexing the record causes an error, if noWarn is true the error
     * will not be logged.
     */
    inline void deleteDocument(OperationContext* const opCtx,
                               StmtId stmtId,
                               const RecordId& loc,
                               OpDebug* const opDebug,
                               const bool fromMigrate = false,
                               const bool noWarn = false) {
        return this->_impl().deleteDocument(opCtx, stmtId, loc, opDebug, fromMigrate, noWarn);
    }

    /*
     * Inserts all documents inside one WUOW.
     * Caller should ensure vector is appropriately sized for this.
     * If any errors occur (including WCE), caller should retry documents individually.
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     */
    inline Status insertDocuments(OperationContext* const opCtx,
                                  const std::vector<InsertStatement>::const_iterator begin,
                                  const std::vector<InsertStatement>::const_iterator end,
                                  OpDebug* const opDebug,
                                  const bool enforceQuota,
                                  const bool fromMigrate = false) {
        return this->_impl().insertDocuments(opCtx, begin, end, opDebug, enforceQuota, fromMigrate);
    }

    /**
     * this does NOT modify the doc before inserting
     * i.e. will not add an _id field for documents that are missing it
     *
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * 'enforceQuota' If false, quotas will be ignored.
     */
    inline Status insertDocument(OperationContext* const opCtx,
                                 const InsertStatement& doc,
                                 OpDebug* const opDebug,
                                 const bool enforceQuota,
                                 const bool fromMigrate = false) {
        return this->_impl().insertDocument(opCtx, doc, opDebug, enforceQuota, fromMigrate);
    }

    /**
     * Callers must ensure no document validation is performed for this collection when calling
     * this method.
     */
    inline Status insertDocumentsForOplog(OperationContext* const opCtx,
                                          const DocWriter* const* const docs,
                                          const size_t nDocs) {
        return this->_impl().insertDocumentsForOplog(opCtx, docs, nDocs);
    }

    /**
     * Inserts a document into the record store and adds it to the MultiIndexBlocks passed in.
     *
     * NOTE: It is up to caller to commit the indexes.
     */
    inline Status insertDocument(OperationContext* const opCtx,
                                 const BSONObj& doc,
                                 const std::vector<MultiIndexBlock*>& indexBlocks,
                                 const bool enforceQuota) {
        return this->_impl().insertDocument(opCtx, doc, indexBlocks, enforceQuota);
    }

    /**
     * Updates the document @ oldLocation with newDoc.
     *
     * If the document fits in the old space, it is put there; if not, it is moved.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
     * @return the post update location of the doc (may or may not be the same as oldLocation)
     */
    inline StatusWith<RecordId> updateDocument(OperationContext* const opCtx,
                                               const RecordId& oldLocation,
                                               const Snapshotted<BSONObj>& oldDoc,
                                               const BSONObj& newDoc,
                                               const bool enforceQuota,
                                               const bool indexesAffected,
                                               OpDebug* const opDebug,
                                               OplogUpdateEntryArgs* const args) {
        return this->_impl().updateDocument(
            opCtx, oldLocation, oldDoc, newDoc, enforceQuota, indexesAffected, opDebug, args);
    }

    inline bool updateWithDamagesSupported() const {
        return this->_impl().updateWithDamagesSupported();
    }

    /**
     * Not allowed to modify indexes.
     * Illegal to call if updateWithDamagesSupported() returns false.
     * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
     * success.
     * @return the contents of the updated record.
     */
    inline StatusWith<RecordData> updateDocumentWithDamages(
        OperationContext* const opCtx,
        const RecordId& loc,
        const Snapshotted<RecordData>& oldRec,
        const char* const damageSource,
        const mutablebson::DamageVector& damages,
        OplogUpdateEntryArgs* const args) {
        return this->_impl().updateDocumentWithDamages(
            opCtx, loc, oldRec, damageSource, damages, args);
    }

    // -----------

    inline StatusWith<CompactStats> compact(OperationContext* const opCtx,
                                            const CompactOptions* const options) {
        return this->_impl().compact(opCtx, options);
    }

    /**
     * removes all documents as fast as possible
     * indexes before and after will be the same
     * as will other characteristics.
     */
    inline Status truncate(OperationContext* const opCtx) {
        return this->_impl().truncate(opCtx);
    }

    /**
     * @return OK if the validate run successfully
     *         OK will be returned even if corruption is found
     *         deatils will be in result.
     */
    inline Status validate(OperationContext* const opCtx,
                           const ValidateCmdLevel level,
                           ValidateResults* const results,
                           BSONObjBuilder* const output) {
        return this->_impl().validate(opCtx, level, results, output);
    }

    /**
     * forces data into cache.
     */
    inline Status touch(OperationContext* const opCtx,
                        const bool touchData,
                        const bool touchIndexes,
                        BSONObjBuilder* const output) const {
        return this->_impl().touch(opCtx, touchData, touchIndexes, output);
    }

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     */
    inline void cappedTruncateAfter(OperationContext* const opCtx,
                                    const RecordId end,
                                    const bool inclusive) {
        return this->_impl().cappedTruncateAfter(opCtx, end, inclusive);
    }

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    inline StatusWithMatchExpression parseValidator(const BSONObj& validator) const {
        return this->_impl().parseValidator(validator);
    }

    static StatusWith<ValidationLevel> parseValidationLevel(StringData);
    static StatusWith<ValidationAction> parseValidationAction(StringData);

    static void registerParseValidationLevelImpl(
        stdx::function<decltype(parseValidationLevel)> impl);
    static void registerParseValidationActionImpl(
        stdx::function<decltype(parseValidationAction)> impl);

    /**
     * Sets the validator for this collection.
     *
     * An empty validator removes all validation.
     * Requires an exclusive lock on the collection.
     */
    inline Status setValidator(OperationContext* const opCtx, const BSONObj validator) {
        return this->_impl().setValidator(opCtx, validator);
    }

    inline Status setValidationLevel(OperationContext* const opCtx, const StringData newLevel) {
        return this->_impl().setValidationLevel(opCtx, newLevel);
    }
    inline Status setValidationAction(OperationContext* const opCtx, const StringData newAction) {
        return this->_impl().setValidationAction(opCtx, newAction);
    }

    inline StringData getValidationLevel() const {
        return this->_impl().getValidationLevel();
    }
    inline StringData getValidationAction() const {
        return this->_impl().getValidationAction();
    }

    // -----------

    //
    // Stats
    //

    inline bool isCapped() const {
        return this->_impl().isCapped();
    }

    /**
     * Get a pointer to a capped insert notifier object. The caller can wait on this object
     * until it is notified of a new insert into the capped collection.
     *
     * It is invalid to call this method unless the collection is capped.
     */
    inline std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const {
        return this->_impl().getCappedInsertNotifier();
    }

    inline uint64_t numRecords(OperationContext* const opCtx) const {
        return this->_impl().numRecords(opCtx);
    }

    inline uint64_t dataSize(OperationContext* const opCtx) const {
        return this->_impl().dataSize(opCtx);
    }

    inline int averageObjectSize(OperationContext* const opCtx) const {
        uint64_t n = this->numRecords(opCtx);

        if (n == 0)
            return 5;
        return static_cast<int>(this->dataSize(opCtx) / n);
    }

    inline uint64_t getIndexSize(OperationContext* const opCtx,
                                 BSONObjBuilder* const details = nullptr,
                                 const int scale = 1) {
        return this->_impl().getIndexSize(opCtx, details, scale);
    }

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must error.
     */
    inline boost::optional<SnapshotName> getMinimumVisibleSnapshot() {
        return this->_impl().getMinimumVisibleSnapshot();
    }

    inline void setMinimumVisibleSnapshot(const SnapshotName name) {
        return this->_impl().setMinimumVisibleSnapshot(name);
    }

    /**
     * Notify (capped collection) waiters of data changes, like an insert.
     */
    inline void notifyCappedWaitersIfNeeded() {
        return this->_impl().notifyCappedWaitersIfNeeded();
    }

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    inline const CollatorInterface* getDefaultCollator() const {
        return this->_impl().getDefaultCollator();
    }

private:
    inline DatabaseCatalogEntry* dbce() const {
        return this->_impl().dbce();
    }

    inline CollectionCatalogEntry* details() const {
        return this->_impl().details();
    }

    inline Status aboutToDeleteCapped(OperationContext* const opCtx,
                                      const RecordId& loc,
                                      const RecordData data) final {
        return this->_impl().aboutToDeleteCapped(opCtx, loc, data);
    }

    inline Status recordStoreGoingToUpdateInPlace(OperationContext* const opCtx,
                                                  const RecordId& loc) final {
        return this->_impl().recordStoreGoingToUpdateInPlace(opCtx, loc);
    }

    // This structure exists to give us a customization point to decide how to force users of this
    // class to depend upon the corresponding `collection.cpp` Translation Unit (TU).  All public
    // forwarding functions call `_impl(), and `_impl` creates an instance of this structure.
    struct TUHook {
        static void hook() noexcept;

        explicit inline TUHook() noexcept {
            if (kDebugBuild)
                this->hook();
        }
    };

    inline const Impl& _impl() const {
        TUHook{};
        return *this->_pimpl;
    }

    inline Impl& _impl() {
        TUHook{};
        return *this->_pimpl;
    }

    std::unique_ptr<Impl> _pimpl;

    friend class DatabaseImpl;
    friend class IndexCatalogImpl;
};
}  // namespace mongo
