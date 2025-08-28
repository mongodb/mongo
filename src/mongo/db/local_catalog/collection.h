/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_options_gen.h"
#include "mongo/db/local_catalog/durable_catalog_entry.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/mixed_schema_buckets_state.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/yieldable.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Holds information update an update operation.
 */
struct CollectionUpdateArgs {
    enum class StoreDocOption { None, PreImage, PostImage };

    CollectionUpdateArgs() = delete;
    CollectionUpdateArgs(BSONObj preImageDoc_) : preImageDoc(preImageDoc_) {}

    std::vector<StmtId> stmtIds = {kUninitializedStmtId};

    // The unique sample id for this update if it has been chosen for sampling.
    boost::optional<UUID> sampleId;

    // The document before modifiers were applied.
    const BSONObj preImageDoc;

    // Fully updated document with damages (update modifiers) applied.
    BSONObj updatedDoc;

    // Document describing the update.
    BSONObj update;

    // Document containing the _id field of the doc being updated.
    BSONObj criteria;

    // Type of update. See OperationSource definition for more details.
    OperationSource source = OperationSource::kStandard;

    StoreDocOption storeDocOption = StoreDocOption::None;
    bool changeStreamPreAndPostImagesEnabledForCollection = false;

    bool retryableWrite = false;

    // Set if the diff insert operation needs to check for the field's existence.
    bool mustCheckExistenceForInsertOperations = false;

    // If not null, stores the replicated RecordId of the document - the RecordId is expected to
    // replicated across nodes with the update.
    RecordId replicatedRecordId;

    // Set if OpTimes were reserved for the update ahead of time.
    std::vector<OplogSlot> oplogSlots;
};

/**
 * Local catalog ('_mdb_catalog') information identifying where collection contents are/should be
 * stored at the time of collection creation.
 */
struct CreateCollCatalogIdentifier {
    /**
     * Where collection information is stored in the local catalog.
     */
    RecordId catalogId;

    /**
     * The main 'ident' associated with a collection.
     */
    std::string ident;

    /**
     * Present when the '_id_' index is automatically generated during collection creation.
     * Otherwise, boost::none.
     */
    boost::optional<std::string> idIndexIdent;

    /**
     * True if 'directoryPerDB' storage option is set.
     */
    bool directoryPerDB = false;

    /**
     * True if 'directoryForIndexes' storage option is set.
     */
    bool directoryForIndexes = false;
};

/**
 * A decorable object that is shared across all Collection instances for the same collection. There
 * may be several Collection instances simultaneously in existence representing different versions
 * of a collection's persisted state. A single instance of SharedCollectionDecorations will be
 * associated with all of the Collection instances for a collection, sharing whatever data may
 * decorate it across all point in time views of the collection.
 */
class SharedCollectionDecorations : public Decorable<SharedCollectionDecorations> {
public:
    SharedCollectionDecorations() = default;
    SharedCollectionDecorations(const SharedCollectionDecorations&) = delete;
    SharedCollectionDecorations& operator=(const SharedCollectionDecorations&) = delete;
};

class Collection : public Decorable<Collection> {
public:
    /**
     * A Collection::Factory is a factory class that constructs Collection objects.
     */
    class Factory {
    public:
        Factory() = default;
        virtual ~Factory() = default;

        static Factory* get(ServiceContext* service);
        static Factory* get(OperationContext* opCtx);
        static void set(ServiceContext* service, std::unique_ptr<Factory> factory);
        /**
         * Constructs a Collection object. This does not persist any state to the storage engine,
         * only constructs an in-memory representation of what already exists on disk.
         */
        virtual std::shared_ptr<Collection> make(
            OperationContext* opCtx,
            const NamespaceString& nss,
            RecordId catalogId,
            std::shared_ptr<durable_catalog::CatalogEntryMetaData> metadata,
            std::unique_ptr<RecordStore> rs) const = 0;
    };

    /**
     * A Collection::Validator represents a filter that is applied to all documents that are
     * inserted. Enforcement of Validators being well formed is done lazily, so the 'Validator'
     * class may represent a validator which is not well formed.
     */
    struct Validator {

        /**
         * Returns whether the validator's filter is well formed.
         */
        bool isOK() const {
            return filter.isOK();
        }

        /**
         * Returns OK or the error encounter when parsing the validator.
         */
        Status getStatus() const {
            return filter.getStatus();
        }

        /**
         * Empty means no validator. This must outlive 'filter'.
         */
        BSONObj validatorDoc;

        /**
         * A special ExpressionContext used to evaluate the filter match expression. This should
         * outlive 'filter'.
         */
        boost::intrusive_ptr<ExpressionContext> expCtxForFilter;

        /**
         * The collection validator MatchExpression. This is stored as a StatusWith, as we lazily
         * enforce that collection validators are well formed.
         *
         * -A non-OK Status indicates that the validator is not well formed, and any attempts to
         * enforce the validator should error.
         *
         * -A value of Status::OK/nullptr indicates that there is no validator.
         *
         * -Anything else indicates a well formed validator. The MatchExpression will maintain
         * pointers into _validatorDoc.
         *
         * Note: this is shared state across cloned Collection instances
         */
        StatusWith<std::shared_ptr<MatchExpression>> filter = {nullptr};
    };

    Collection() = default;
    ~Collection() override = default;

    /**
     * Clones this Collection instance. Some members are deep copied and some are shallow copied.
     * This should only be be called from the CollectionCatalog when it needs a writable collection.
     */
    virtual std::shared_ptr<Collection> clone() const = 0;

    /**
     * Fetches the shared state across Collection instances for the a collection. Returns an object
     * decorated by state shared across Collection instances for the same namespace. Its decorations
     * are unversioned (not associated with any point in time view of the collection) data related
     * to the collection.
     */
    virtual SharedCollectionDecorations* getSharedDecorations() const = 0;

    virtual void init(OperationContext* opCtx) {}

    /**
     * Initializes a collection representative at the provided read timestamp using the shared state
     * from an already existing, later collection.
     *
     * Returns the SnapshotTooOld error if the underlying data files have already been removed.
     */
    virtual Status initFromExisting(OperationContext* opCtx,
                                    const std::shared_ptr<const Collection>& collection,
                                    const durable_catalog::CatalogEntry& catalogEntry,
                                    boost::optional<Timestamp> readTimestamp) = 0;

    virtual bool isInitialized() const {
        return false;
    }

    virtual const NamespaceString& ns() const = 0;

    /**
     * Sets a new namespace on this Collection, in the case that the Collection is being renamed.
     * In general, reads and writes to Collection objects are synchronized using locks from the lock
     * manager. However, there is special synchronization for ns() and rename() so that the
     * CollectionCatalog can perform UUID to namespace lookup without holding a Collection lock. See
     * CollectionCatalog::onCollectionRename().
     */
    virtual Status rename(OperationContext* opCtx, const NamespaceString& nss, bool stayTemp) = 0;

    virtual RecordId getCatalogId() const = 0;

    virtual UUID uuid() const = 0;

    virtual const IndexCatalog* getIndexCatalog() const = 0;
    virtual IndexCatalog* getIndexCatalog() = 0;

    virtual RecordStore* getRecordStore() const = 0;

    /**
     * Fetches the Ident for this collection.
     */
    virtual std::shared_ptr<Ident> getSharedIdent() const = 0;

    /**
     * Sets the Ident for this collection.
     */
    virtual void setIdent(std::shared_ptr<Ident> newIdent) = 0;

    virtual BSONObj getValidatorDoc() const = 0;

    /**
     * Returns 'kPass' with an ok Status if the document passes this collection's schema validator.
     *
     * Returns a non-ok Status if the document does not pass this collection's schema validator and
     * returns 'kWarn', 'kError' or 'kErrorAndLog' based on the validation action.
     *
     * The validation action is set during collection creation and can be modified as part of
     * collMod. It determines whether we should error for documents that violate the schema
     * validation rules, or warn about, but allow invalid documents.
     */
    enum class SchemaValidationResult { kPass, kWarn, kError, kErrorAndLog };
    virtual std::pair<SchemaValidationResult, Status> checkValidation(
        OperationContext* opCtx, const BSONObj& document) const = 0;

    /**
     * Extension of `checkValidation` above which converts the tri-modal return value into either a
     * successful or failed status, printing warning if necessary.
     */
    virtual Status checkValidationAndParseResult(OperationContext* opCtx,
                                                 const BSONObj& document) const = 0;

    virtual bool requiresIdIndex() const = 0;

    virtual Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const = 0;

    /**
     * @param out - contents set to the right docs if exists, or nothing.
     * @return true iff loc exists
     */
    virtual bool findDoc(OperationContext* opCtx,
                         const RecordId& loc,
                         Snapshotted<BSONObj>* out) const = 0;

    virtual std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                            bool forward = true) const = 0;

    virtual bool updateWithDamagesSupported() const = 0;


    // -----------

    /**
     * removes all documents as fast as possible
     * indexes before and after will be the same
     * as will other characteristics.
     *
     * The caller should hold a collection X lock and ensure there are no index builds in progress
     * on the collection.
     */
    virtual Status truncate(OperationContext* opCtx) = 0;

    /**
     * Returns a non-ok Status if validator is not legal for this collection.
     */
    virtual Validator parseValidator(
        OperationContext* opCtx,
        const BSONObj& validator,
        MatchExpressionParser::AllowedFeatureSet allowedFeatures) const = 0;

    /**
     * Sets the validator for this collection.
     *
     * An empty validator removes all validation.
     * Requires an exclusive lock on the collection.
     */
    virtual void setValidator(OperationContext* opCtx, Validator validator) = 0;

    virtual Status setValidationLevel(OperationContext* opCtx, ValidationLevelEnum newLevel) = 0;
    virtual Status setValidationAction(OperationContext* opCtx, ValidationActionEnum newAction) = 0;

    virtual boost::optional<ValidationLevelEnum> getValidationLevel() const = 0;
    virtual boost::optional<ValidationActionEnum> getValidationAction() const = 0;

    virtual Status updateValidator(OperationContext* opCtx,
                                   BSONObj newValidator,
                                   boost::optional<ValidationLevelEnum> newLevel,
                                   boost::optional<ValidationActionEnum> newAction) = 0;

    virtual Status checkValidatorAPIVersionCompatability(OperationContext* opCtx) const = 0;

    virtual bool isChangeStreamPreAndPostImagesEnabled() const = 0;
    virtual void setChangeStreamPreAndPostImages(OperationContext* opCtx,
                                                 ChangeStreamPreAndPostImagesOptions val) = 0;

    /**
     * Returns true if this is a time-series collection.
     */
    virtual bool isTimeseriesCollection() const = 0;

    /**
     * return true if this is a time-series collection with FCV 9.0 metadata format,
     * the time-series collection has a unified namespace (no view nor bucket collection, only the
     * main namespace)
     *
     * TODO SERVER-101588: remove this function and all its usages once 9.0 becomes last LTS.
     * By then only viewless timeseries collection will exists.
     */
    virtual bool isNewTimeseriesWithoutView() const = 0;

    /**
     * Returns true if this is a temporary collection.
     */
    virtual bool isTemporary() const = 0;

    /**
     * Returns a description of whether time-series mixed-schema buckets may be present for this
     * collection, and how they should be handled.
     */
    virtual timeseries::MixedSchemaBucketsState getTimeseriesMixedSchemaBucketsState() const = 0;

    /**
     * Sets the 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag to 'setting' for this
     * collection.
     *
     * Throws if this is not a time-series collection.
     */
    virtual void setTimeseriesBucketsMayHaveMixedSchemaData(OperationContext* opCtx,
                                                            boost::optional<bool> setting) = 0;

    virtual boost::optional<bool> timeseriesBucketingParametersHaveChanged() const = 0;

    /**
     * Sets the 'timeseriesBucketingParametersHaveChanged' catalog entry flag to 'value' for this
     * collection.
     *
     * Throws if this is not a time-series collection.
     */
    virtual void setTimeseriesBucketingParametersChanged(OperationContext* opCtx,
                                                         boost::optional<bool> value) = 0;

    /**
     * Used to remove the legacy `md.timeseriesBucketingParametersHaveChanged` catalog field
     * during FCV upgrade.
     */
    // TODO(SERVER-101423): Remove once 9.0 becomes last LTS.
    virtual void removeLegacyTimeseriesBucketingParametersHaveChanged(OperationContext* opCtx) = 0;

    /**
     * Returns true if the passed in time-series bucket document contains mixed-schema data. Returns
     * a non-OK status if the bucket's min/max is malformed.
     */
    virtual StatusWith<bool> doesTimeseriesBucketsDocContainMixedSchemaData(
        const BSONObj& bucketsDoc) const = 0;

    /**
     * Returns true if the time-series collection may have dates outside the standard range (roughly
     * 1970-2038). The value may be updated in the background by another thread between calls, even
     * if the caller holds a lock on the collection. The value may only transition from false to
     * true.
     */
    virtual bool getRequiresTimeseriesExtendedRangeSupport() const = 0;

    /**
     * Sets the in-memory flag for this collection. This value can be retrieved by
     * 'getRequiresTimeseriesExtendedRangeSupport'.
     *
     * Throws if this is not a time-series collection.
     */
    virtual void setRequiresTimeseriesExtendedRangeSupport(OperationContext* opCtx) const = 0;

    /**
     * Returns true if the time-series collection was created with 'bucketRoundingSeconds' and
     * 'bucketMaxSpanSeconds', and none of the time-series options have been changed. The value may
     * only transition from true to false.
     */
    virtual bool areTimeseriesBucketsFixed() const = 0;

    /*
     * Returns true if this collection is clustered. That is, its RecordIds store the value of the
     * cluster key. If the collection is clustered on _id, there is no separate _id index.
     */
    virtual bool isClustered() const = 0;

    virtual boost::optional<ClusteredCollectionInfo> getClusteredInfo() const = 0;

    /**
     * Updates the expireAfterSeconds setting for a clustered TTL index in this Collection and the
     * durable catalog.
     */
    virtual void updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                                boost::optional<int64_t> expireAfterSeconds) = 0;

    virtual Status updateCappedSize(OperationContext* opCtx,
                                    boost::optional<long long> newCappedSize,
                                    boost::optional<long long> newCappedMax) = 0;

    /**
     * Updates the 'recordIdsReplicated' setting to false.
     * The collection will no longer guarantee the same recordId for a given document across all
     * nodes in the a replica set.
     */
    virtual void unsetRecordIdsReplicated(OperationContext* opCtx) = 0;

    //
    // Index
    //

    /**
     * Checks that the metadata for the index exists and matches the given spec.
     * Returns offset in metadata on success.
     */
    virtual StatusWith<int> checkMetaDataForIndex(const std::string& indexName,
                                                  const BSONObj& spec) const = 0;

    /*
     * Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    virtual void updateTTLSetting(OperationContext* opCtx,
                                  StringData idxName,
                                  long long newExpireSeconds) = 0;

    /*
     * Hide or unhide the given index. A hidden index will not be considered for use by the
     * query planner.
     */
    virtual void updateHiddenSetting(OperationContext* opCtx, StringData idxName, bool hidden) = 0;

    /*
     * Converts the the given index to be unique or non-unique.
     */
    virtual void updateUniqueSetting(OperationContext* opCtx, StringData idxName, bool unique) = 0;

    /*
     * Disallows or allows new duplicates in the given index.
     */
    virtual void updatePrepareUniqueSetting(OperationContext* opCtx,
                                            StringData idxName,
                                            bool prepareUnique) = 0;

    /**
     * Repairs invalid index options on all indexes in this collection. Returns a list of
     * index names that were repaired. Specifying 'removeDeprecatedFields' as true, causes
     * deprecated fields, which much be otherwise supported for backwards compatibility, to be
     * removed when performing the repair.
     */
    virtual std::vector<std::string> repairInvalidIndexOptions(
        OperationContext* opCtx, bool removeDeprecatedFields = false) = 0;

    /**
     * Updates the 'temp' setting for this collection.
     */
    virtual void setIsTemp(OperationContext* opCtx, bool isTemp) = 0;

    /**
     * Removes the index 'indexName' from the persisted collection catalog entry identified by
     * 'catalogId'.
     */
    virtual void removeIndex(OperationContext* opCtx, StringData indexName) = 0;

    /**
     * Updates the persisted catalog entry for 'ns' with the new index and creates the index on
     * disk.
     *
     * A passed 'buildUUID' implies that the index is part of a two-phase index build.
     */
    virtual Status prepareForIndexBuild(OperationContext* opCtx,
                                        const IndexDescriptor* spec,
                                        StringData indexIdent,
                                        boost::optional<UUID> buildUUID) = 0;

    /**
     * Returns a UUID if the index is being built with the two-phase index build procedure.
     */
    virtual boost::optional<UUID> getIndexBuildUUID(StringData indexName) const = 0;

    /**
     * Returns true if the index identified by 'indexName' is multikey, and returns false otherwise.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information in the catalog, then this
     * function sets 'multikeyPaths' as the path components that cause this index to be multikey.
     *
     * In particular, if this function returns false and the index supports tracking path-level
     * multikey information, then 'multikeyPaths' is initialized as a vector with size equal to the
     * number of elements in the index key pattern of empty sets.
     */
    virtual bool isIndexMultikey(OperationContext* opCtx,
                                 StringData indexName,
                                 MultikeyPaths* multikeyPaths,
                                 int indexOffset = -1) const = 0;

    /**
     * Sets the index identified by 'indexName' to be multikey.
     *
     * If 'multikeyPaths' is non-empty, then it must be a vector with size equal to the number of
     * elements in the index key pattern. Additionally, at least one path component of the indexed
     * fields must cause this index to be multikey.
     *
     * This function returns true if the index metadata has changed, and returns false otherwise.
     */
    virtual bool setIndexIsMultikey(OperationContext* opCtx,
                                    StringData indexName,
                                    const MultikeyPaths& multikeyPaths,
                                    int indexOffset = -1) const = 0;

    /**
     * Sets the index to be multikey with the provided paths. This performs minimal validation of
     * the inputs and is intended to be used internally to "correct" multikey metadata that drifts
     * from the underlying collection data.
     *
     * When isMultikey is false, ignores multikeyPaths and resets the metadata appropriately based
     * on the index descriptor. Otherwise, overwrites the existing multikeyPaths with the ones
     * provided. This only writes multikey paths if the index type supports path-level tracking, and
     * only sets the multikey boolean flag otherwise.
     */
    virtual void forceSetIndexIsMultikey(OperationContext* opCtx,
                                         const IndexDescriptor* desc,
                                         bool isMultikey,
                                         const MultikeyPaths& multikeyPaths) const = 0;

    virtual int getTotalIndexCount() const = 0;

    virtual int getCompletedIndexCount() const = 0;

    virtual BSONObj getIndexSpec(StringData indexName) const = 0;

    virtual void getAllIndexes(std::vector<std::string>* names) const = 0;

    virtual void getReadyIndexes(std::vector<std::string>* names) const = 0;

    virtual bool isIndexPresent(StringData indexName) const = 0;

    virtual bool isIndexReady(StringData indexName) const = 0;

    virtual void replaceMetadata(OperationContext* opCtx,
                                 std::shared_ptr<durable_catalog::CatalogEntryMetaData> md) = 0;

    virtual bool isMetadataEqual(const BSONObj& otherMetadata) const = 0;

    /**
     * Specifies whether writes to this collection should X-lock the metadata resource. It is only
     * set for replicated, non-clustered capped collections. Such collections require writes to be
     * serialized on the secondary in order to guarantee insertion order (SERVER-21483). This
     * exclusive access to the metadata resource prevents the primary from executing with more
     * concurrency than secondaries - thus helping secondaries keep up, and protects the
     * 'cappedFirstRecord' value for the collection. See SERVER-21646.
     *
     * On the other hand, capped clustered collections with a monotonically increasing cluster key
     * natively guarantee preservation of the insertion order, and don't need serialisation, so we
     * allow concurrent inserts for clustered capped collections.
     */
    virtual bool needsCappedLock() const = 0;

    /**
     * Checks whether the collection is capped and if the current data size or number of records
     * exceeds cappedMaxSize or cappedMaxDocs respectively.
     */
    virtual bool isCappedAndNeedsDelete(OperationContext* opCtx) const = 0;

    //
    // Stats
    //

    virtual bool areRecordIdsReplicated() const = 0;

    virtual bool isCapped() const = 0;
    virtual long long getCappedMaxDocs() const = 0;
    virtual long long getCappedMaxSize() const = 0;

    virtual long long numRecords(OperationContext* opCtx) const = 0;

    /**
     * Return uncompressed collection data size in bytes
     */
    virtual long long dataSize(OperationContext* opCtx) const = 0;

    /**
     * Return the size on disk in bytes
     */
    virtual int64_t sizeOnDisk(OperationContext* opCtx,
                               const StorageEngine& storageEngine) const = 0;

    /**
     * Returns true if the collection does not contain any records.
     */
    virtual bool isEmpty(OperationContext* opCtx) const = 0;

    /**
     * Return the average object size in bytes
     */
    virtual int averageObjectSize(OperationContext* opCtx) const = 0;

    virtual uint64_t getIndexSize(OperationContext* opCtx,
                                  BSONObjBuilder* details = nullptr,
                                  int scale = 1) const = 0;

    /**
     * Returns the number of unused, free bytes used by all indexes on disk.
     */
    virtual uint64_t getIndexFreeStorageBytes(OperationContext* opCtx) const = 0;

    /**
     * Get the timestamp this Collection instance was most recently changed at.
     */
    virtual boost::optional<Timestamp> getMinimumValidSnapshot() const = 0;

    /**
     * Sets the timestamp this Collection instance was most recently changed at.
     */
    virtual void setMinimumValidSnapshot(Timestamp name) = 0;

    /**
     * Returns the time-series options for this buckets collection, or boost::none if not a
     * time-series buckets collection.
     */
    virtual const boost::optional<TimeseriesOptions>& getTimeseriesOptions() const = 0;

    /**
     * Sets the time-series options for this buckets collection.
     */
    virtual void setTimeseriesOptions(OperationContext* opCtx,
                                      const TimeseriesOptions& tsOptions) = 0;

    /**
     * Get a pointer to the collection's default collator. The pointer must not be used after this
     * Collection is destroyed.
     */
    virtual const CollatorInterface* getDefaultCollator() const = 0;

    /**
     * Returns a cached version of the Collection MetaData that matches the version of this
     * Collection instance.
     */
    virtual const CollectionOptions& getCollectionOptions() const = 0;

    /**
     * Fills in each index specification with collation information from this collection and returns
     * the new index specifications.
     *
     * The returned index specifications will not be equivalent to the ones specified in
     * 'indexSpecs' if any missing collation information were filled in; however, the returned index
     * specifications will match the form stored in the IndexCatalog should any of these indexes
     * already exist.
     */
    virtual StatusWith<BSONObj> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const BSONObj& indexSpec) const = 0;
    virtual StatusWith<std::vector<BSONObj>> addCollationDefaultsToIndexSpecsForCreate(
        OperationContext* opCtx, const std::vector<BSONObj>& indexSpecs) const = 0;

    virtual void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) = 0;

    /**
     * Called when this Collection is deregistered from the catalog
     */
    virtual void onDeregisterFromCatalog(ServiceContext* svcCtx) = 0;

    friend auto logAttrs(const Collection& col) {
        return logv2::multipleAttrs(col.ns(), col.uuid());
    }
};

/**
 * A collection pointer that is consistent with the underlying WT snapshot. This class will
 * invariant in debug builds that there are no pointers to the collection leftover once we abandon
 * the snapshot. In release mode this class is equivalent to the plain pointer to avoid the
 * performance overhead of reference counting.
 *
 * A proper instance of this class should only be constructed by trusted users such as the
 * CollectionCatalog or the CollectionPtr to avoid misuse.
 *
 * Note that this class cannot detect an ABA problem with respect to collections acquired under a
 * lock. The following scenario is possible and won't be caught by this class:
 * - Lock on collA is acquired
 * - ConsistentCollection on collA is created
 * - Lock is released
 * - Lock is acquired again
 * - ConsistentCollection is used
 * This usage will yield an invalid ConsistentCollection since the Collection could've potentially
 * been modified but won't be detected.
 *
 * TODO SERVER-95260: Investigate if this can be detected once generational lock information is
 * available.
 */
class ConsistentCollection {
public:
    ConsistentCollection() = default;

#ifdef MONGO_CONFIG_DEBUG_BUILD
    ~ConsistentCollection();

    ConsistentCollection(ConsistentCollection&& other);
    ConsistentCollection(const ConsistentCollection& other);

    ConsistentCollection& operator=(ConsistentCollection&& other);
    ConsistentCollection& operator=(const ConsistentCollection& other);
#else
    ~ConsistentCollection() = default;

    ConsistentCollection(ConsistentCollection&& other) = default;
    ConsistentCollection(const ConsistentCollection& other) = default;

    ConsistentCollection& operator=(ConsistentCollection&& other) = default;
    ConsistentCollection& operator=(const ConsistentCollection& other) = default;
#endif

    operator bool() const {
        return static_cast<bool>(_collection);
    }

    const Collection* operator->() const {
        return get();
    }

    const Collection* get() const {
        return _collection;
    }

private:
    friend class CollectionCatalog;  // The catalog is the main source for consistent collections as
                                     // part of establishing a consistent collection with the WT
                                     // snapshot.

    // TODO(SERVER-103415): revisit this friendship after creating a non-const ctor for
    // CollectionPtr
    friend class CollectionPtr;  // CollectionPtr can manually construct a consistent collection in
                                 // order to maintain compatibility with legacy usages.
    friend class LockedCollectionYieldRestore;  // Locked collections implicitly guarantee that the
                                                // collection is consistent with the snapshot since
                                                // they impede all modifications to it.
    friend class CatalogTestFixture;

    const Collection* _collection = nullptr;

#ifdef MONGO_CONFIG_DEBUG_BUILD
    RecoveryUnit::Snapshot* _snapshot = nullptr;

    ConsistentCollection(OperationContext* opCtx, const Collection* collection);

    int _getRefCount() const;

    void _releaseCollectionIfInSnapshot();
#else
    ConsistentCollection(OperationContext* opCtx, const Collection* collection)
        : _collection(collection) {}
#endif
};

/**
 * Smart-pointer'esque type to handle yielding of Collection lock that may invalidate pointers when
 * resuming. CollectionPtr will re-load the Collection from the Catalog when restoring from a yield
 * that dropped locks.
 */
class CollectionPtr : public Yieldable {
public:
    static CollectionPtr null;

    // Function for the implementation on how we load a new Collection pointer when restoring from
    // yield
    using RestoreFn = std::function<ConsistentCollection(OperationContext*, boost::optional<UUID>)>;

    // Creates non-yieldable CollectionPtr, performing yield/restore will invariant. To make this
    // CollectionPtr yieldable call `makeYieldable` and provide appropriate implementation depending
    // on context.
    CollectionPtr() = default;
    explicit CollectionPtr(ConsistentCollection collection);

    // TODO SERVER-103415: Remove this unsafe function
    [[deprecated(
        "This function is deprecated as the Collection* could go stale. Please use the "
        "CollectionPtr(ConsistentCollection collection) constructor through the acquisition API "
        "instead.")]] static CollectionPtr
    CollectionPtr_UNSAFE(const Collection* coll);

    // As opposed to constructing a CollectionPtr with a const Collection*, this constructor is safe
    // because a non-const collection is guaranteed to be protected by a MODE_X lock. This means
    // that the collection instance cannot be deleted while the CollectionPtr is alive and cannot be
    // stale.
    CollectionPtr(Collection* coll);
    CollectionPtr(const CollectionPtr&) = delete;
    CollectionPtr(CollectionPtr&&);
    ~CollectionPtr() override;

    CollectionPtr& operator=(const CollectionPtr&) = delete;
    CollectionPtr& operator=(CollectionPtr&&);

    explicit operator bool() const {
        return static_cast<bool>(_collection);
    }

    bool operator==(const CollectionPtr& other) const {
        return get() == other.get();
    }
    bool operator!=(const CollectionPtr& other) const {
        return !operator==(other);
    }
    const Collection* operator->() const {
        return _collection.get();
    }
    const Collection* get() const {
        return _collection.get();
    }

    // Makes this CollectionPtr yieldable. The RestoreFn provides an implementation on how to setup
    // the correct state after yield and fetches the internal Collection pointer.
    void makeYieldable(OperationContext* opCtx, RestoreFn restoreFn) {
        _opCtx = opCtx;
        _restoreFn = std::move(restoreFn);
    }

    void reset() {
        *this = CollectionPtr();
    }

    bool yieldable() const override;
    void yield() const override;
    void restore() const override;

    friend std::ostream& operator<<(std::ostream& os, const CollectionPtr& coll);

    void setShardKeyPattern(const BSONObj& shardKeyPattern);
    const ShardKeyPattern& getShardKeyPattern() const;

    // TODO SERVER-79296 remove sharding info from CollectionPtr
    bool isSharded_DEPRECATED() const {
        return static_cast<bool>(_shardKeyPattern);
    }

private:
    // TODO SERVER-103415: Replace this with a non-const version and make it public. Fully owned
    // collections are safe to use since they are not consistent with any snapshot by definition as
    // they are being committed.
    [[deprecated(
        "This constructor is in the process of being replaced with the much safer "
        "ConsistentCollection one. Please try to avoid using it as it could lead to an "
        "inconsistent CollectionPtr in the event of a WT snapshot yield if stored without "
        "care")]] explicit CollectionPtr(const Collection* coll);

    // These members needs to be mutable so the yield/restore interface can be const. We don't want
    // yield/restore to require a non-const instance when it otherwise could be const.
    mutable ConsistentCollection _collection;

    // If the collection is currently in the 'yielded' state (i.e. yield() has been called), this
    // field will contain what was the UUID of the collection at the time of yield.
    mutable boost::optional<UUID> _yieldedUUID;

    // Contains whether we are in the 'yielded' state.
    mutable bool _yielded = false;

    OperationContext* _opCtx = nullptr;
    RestoreFn _restoreFn;

    // Stores a consistent view of shard key with the collection that will be needed during the
    // operation. If _shardKeyPattern is set, that indicates that the collection is sharded.
    boost::optional<ShardKeyPattern> _shardKeyPattern = boost::none;
};

inline std::ostream& operator<<(std::ostream& os, const CollectionPtr& coll) {
    os << coll.get();
    return os;
}

inline ValidationActionEnum validationActionOrDefault(
    boost::optional<ValidationActionEnum> action) {
    return action.value_or(ValidationActionEnum::error);
}

inline ValidationLevelEnum validationLevelOrDefault(boost::optional<ValidationLevelEnum> level) {
    return level.value_or(ValidationLevelEnum::strict);
}

/**
 * Returns a collator for the user-specified collation 'userCollation'.
 *
 * Note: The caller should check if 'userCollation' is not empty since the empty 'userCollation'
 * has the special meaning that the query follows the collection default collation that exists.
 */
inline std::unique_ptr<CollatorInterface> getUserCollator(OperationContext* opCtx,
                                                          const BSONObj& userCollation) {
    tassert(7542402, "Empty user collation", !userCollation.isEmpty());
    return uassertStatusOK(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(userCollation));
}

/**
 * Resolves the collator to either the user-specified collation or, if none was specified, to
 * the collection-default collation and also returns a flag indicating whether the user-provided
 * collation matches the collection default collation.
 */
std::pair<std::unique_ptr<CollatorInterface>, ExpressionContextCollationMatchesDefault>
resolveCollator(OperationContext* opCtx, BSONObj userCollation, const CollectionPtr& collection);

}  // namespace mongo
