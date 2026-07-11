// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_options_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CollatorFactoryInterface;
class CreateCommand;

struct [[MONGO_MOD_PUBLIC]] CollectionOptions {
    /**
     * Returns true if the options indicate the namespace is a view.
     */
    bool isView() const;

    /**
     * The 'uuid' member is a collection property stored in the catalog with user-settable options,
     * but is not valid for the user to specify as collection option. So, parsing commands must
     * reject the 'uuid' property, but parsing stored options must accept it.
     */
    enum ParseKind { parseForCommand, parseForStorage };

    /**
     * Confirms that collection options can be converted to BSON and back without errors.
     */
    Status validateForStorage() const;

    /**
     * Parses the collection 'options' into the appropriate struct fields.
     *
     * When 'kind' is set to ParseKind::parseForStorage, the 'uuid' field is parsed,
     * otherwise the 'uuid' field is not parsed.
     *
     * When 'kind' is set to ParseKind::parseForCommand, the 'idIndex' field is parsed,
     * otherwise the 'idIndex' field is not parsed.
     */
    static StatusWith<CollectionOptions> parse(const BSONObj& options,
                                               ParseKind kind = parseForCommand);

    /**
     * Converts a client "create" command invocation.
     */
    static CollectionOptions fromCreateCommand(OperationContext* opCtx, const CreateCommand& cmd);

    static StatusWith<long long> checkAndAdjustCappedSize(long long cappedSize);
    static StatusWith<long long> checkAndAdjustCappedMaxDocs(long long cappedMaxDocs);

    /**
     * Serialize to BSON. The 'includeUUID' parameter is used for the listCollections command to do
     * special formatting for the uuid. Aside from the UUID, if 'includeFields' is non-empty, only
     * the specified fields will be included.
     */
    void appendBSON(BSONObjBuilder* builder,
                    bool includeUUID,
                    const StringDataSet& includeFields) const;
    BSONObj toBSON(bool includeUUID = true, const StringDataSet& includeFields = {}) const;

    /**
     * Returns true if given options matches to this.
     *
     * Uses the collatorFactory to normalize the collation property being compared.
     *
     * Note: ignores idIndex property.
     */
    bool matchesStorageOptions(const CollectionOptions& other,
                               CollatorFactoryInterface* collatorFactory) const;

    // Collection UUID. If not set, specifies that the storage engine should generate the UUID (for
    // a new collection). For an existing collection parsed for storage, it will always be present.
    boost::optional<UUID> uuid;

    bool capped = false;
    long long cappedSize = 0;
    long long cappedMaxDocs = 0;

    // The behavior of _id index creation when collection created
    void setNoIdIndex() {
        autoIndexId = NO;
    }
    enum {
        DEFAULT,  // currently yes for most collections, NO for some system ones
        YES,      // create _id index
        NO        // do not create _id index
    } autoIndexId = DEFAULT;

    bool temp = false;

    // Change stream options define whether or not to store the pre-images of the documents affected
    // by update and delete operations in a dedicated collection, that will be used for reading data
    // via changeStreams.
    ChangeStreamPreAndPostImagesOptions changeStreamPreAndPostImagesOptions{false};

    // Storage engine collection options. Always owned or empty.
    BSONObj storageEngine;

    // Default options for indexes created on the collection.
    IndexOptionDefaults indexOptionDefaults;

    // Index specs for the _id index.
    BSONObj idIndex;

    // Always owned or empty.
    BSONObj validator;
    boost::optional<ValidationActionEnum> validationAction;
    boost::optional<ValidationLevelEnum> validationLevel;
    bool prepareConstraintValidationLevel = false;

    // The namespace's default collation.
    BSONObj collation;

    // Exists if the collection is clustered.
    boost::optional<ClusteredCollectionInfo> clusteredIndex;

    // If present, the number of seconds after which old data should be deleted. Only for
    // collections which are clustered on _id.
    boost::optional<int64_t> expireAfterSeconds;

    // View-related options.
    // The namespace of the view or collection that "backs" this view, or the empty string if this
    // collection is not a view.
    std::string viewOn;
    // The aggregation pipeline that defines this view.
    BSONObj pipeline;

    // The options that define the time-series collection, or boost::none if not a time-series
    // collection.
    boost::optional<TimeseriesOptions> timeseries;

    // The options for collections with encrypted fields
    boost::optional<EncryptedFieldConfig> encryptedFieldConfig;
};

[[MONGO_MOD_PRIVATE]]
Status validateChangeStreamPreAndPostImagesOptionIsPermitted(const NamespaceString& ns);

}  // namespace mongo
