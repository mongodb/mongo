/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <array>
#include <boost/move/utility_core.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <compare>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_stats.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * ESC Collection schema
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, type || pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  count_type || count)
 * }
 *
 * where
 *  type = uint64_t
 *  pos = uint64_t
 *  count_type = uint64_t
 *  count = uint64_t
 *  - Note: There is a lifetime limit of 2^64 - 1 count of a value/pairs for an index
 *
 * where type
 *   0 - null record
 *   1 - insert record, positional record, or compaction record
 *
 * where count_type:
 *   0 - regular count
 *   [1, UINT64_MAX) = position
 *   UINT64_MAX - compaction placeholder
 *
 * Record types:
 *
 * Document Counts
 * Null: 0 or 1
 * Insert: 0 or more
 * Positional: 0 or more
 * Compaction: 0 or 1
 *
 * Null record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, null )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  pos || count)
 * }
 *
 * Insert record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  0 || count)
 * }
 *
 * Positional record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  pos' || count)
 * }
 *
 * Compaction placeholder record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  UINT64_MAX || 0)
 * }
 *
 * PlainText of _id
 * struct {
 *    uint64_t type;
 *    uint64_t pos;
 * }
 *
 * PlainText of value
 * struct {
 *    uint64_t count_type;
 *    uint64_t count;
 * }
 *
 * ===== Protocol Version 2 =====
 * Positional values:
 *   cpos = position of non-anchor record in the range [1..UINT64_MAX]
 *   apos = position of anchor record in the range [1..UINT64_MAX]
 *
 * Non-anchor record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, cpos)
 * }
 *
 * Non-null anchor record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, (0 || apos))
 *    value : Encrypt(ESCTwiceDerivedValueToken, (0 || cpos))
 * }
 *
 * Null anchor record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, (0 || 0))
 *    value : Encrypt(ESCTwiceDerivedValueToken, (apos || cpos))
 * }
 *
 */


struct ESCNullDocument {
    // Id is not included as it is HMAC generated and cannot be reversed
    uint64_t position;
    uint64_t count;
};


struct ESCDocument {
    // Id is not included as it is HMAC generated and cannot be reversed
    bool compactionPlaceholder;
    uint64_t position;
    uint64_t count;
};


/**
 * Basic set of functions to read/query data from state collections to perform EmuBinary.
 */
class FLETagQueryInterface {
public:
    enum class TagQueryType { kInsert, kQuery, kCompact, kCleanup, kPadding };

    virtual ~FLETagQueryInterface();

    /**
     * Retrieve a single document by _id == BSONElement from nss.
     *
     * Returns an empty BSONObj if no document is found.
     * Expected to throw an error if it detects more then one documents.
     */
    virtual BSONObj getById(const NamespaceString& nss, BSONElement element) = 0;

    /**
     * Count the documents in the collection.
     *
     * Throws if the collection is not found.
     */
    virtual uint64_t countDocuments(const NamespaceString& nss) = 0;

    /**
     * Get the set of counts from ESC for a set of tags. Returns counts for these fields suitable
     * either for query or insert based on the type parameter.
     *
     * Returns a vector of zeros if the collection does not exist.
     */
    virtual std::vector<std::vector<FLEEdgeCountInfo>> getTags(
        const NamespaceString& nss,
        const std::vector<std::vector<FLEEdgePrfBlock>>& tokensSets,
        TagQueryType type) = 0;
};


/**
 * Interface for reading from a collection for the "EmuBinary" algorithm
 */
class FLEStateCollectionReader {
public:
    virtual ~FLEStateCollectionReader() = default;

    /**
     * Get a count of documents in the collection.
     */
    virtual uint64_t getDocumentCount() const = 0;

    /**
     * Get a document by its _id.
     */
    virtual BSONObj getById(PrfBlock block) const = 0;

    /**
     * Return true by a document by _id if it exists.
     */
    virtual bool existsById(PrfBlock block) const {
        return !getById(block).isEmpty();
    }

    virtual ECStats getStats() const = 0;
};

template <class TagToken, class ValueToken>
class ESCCollectionCommon {
public:
    /**
     * Decrypt a regular document.
     */
    static StatusWith<ESCDocument> decryptDocument(const ValueToken& valueToken, BSONObj& doc);

    /**
     * Decrypt a regular document.
     */
    static StatusWith<ESCDocument> decryptDocument(const ValueToken& valueToken, BSONObj&& doc);

    /**
     * Generate the _id value for an anchor record
     */
    static PrfBlock generateAnchorId(const TagToken& tagToken, uint64_t apos);

    /**
     * Generate the _id value for a null anchor record
     */
    static PrfBlock generateNullAnchorId(const TagToken& tagToken);

    /**
     * Calculate AnchorBinaryHops as described in OST.
     */
    static boost::optional<uint64_t> anchorBinaryHops(const FLEStateCollectionReader& reader,
                                                      const TagToken& tagToken,
                                                      const ValueToken& valueToken,
                                                      FLEStatusSection::EmuBinaryTracker& tracker);

    /**
     * Decrypts an anchor document (either null or non-null).
     * If the input document is a non-null anchor, then the resulting ESCDocument.position
     * is 0, and ESCDocument.count is the non-anchor position (cpos).
     * If the input document is a null anchor, then ESCDocument.position is the non-zero
     * anchor position (apos), and ESCDocument.count is the cpos.
     */
    static StatusWith<ESCDocument> decryptAnchorDocument(const ValueToken& valueToken,
                                                         BSONObj& doc);

    /**
     * Reads the anchor document identified by anchorId, and if found, decrypts the value
     * and returns the parsed positions as a pair. If the anchor is not found, returns none.
     */
    static boost::optional<ESCCountsPair> readAndDecodeAnchor(
        const FLEStateCollectionReader& reader,
        const ValueToken& valueToken,
        const PrfBlock& anchorId);

    /**
     * Performs all the ESC reads required by the QE cleanup algorithm, for anchor cleanup or
     * padding cleanup.
     */
    static FLEEdgeCountInfo getEdgeCountInfoForPaddingCleanupCommon(
        const FLEStateCollectionReader& reader,
        const TagToken& tagToken,
        const ValueToken& valueToken,
        const EmuBinaryResult& positions);
};

/**
 * Specialization of ESCollectionCommon for ESCTwiceDerived(Tag|Value)Tokens
 * with additional methods specific to encrypted data.
 */
class ESCCollection
    : public ESCCollectionCommon<ESCTwiceDerivedTagToken, ESCTwiceDerivedValueToken> {
public:
    /**
     * Generate the _id value
     */
    static PrfBlock generateId(const ESCTwiceDerivedTagToken& tagToken,
                               boost::optional<uint64_t> index);

    /**
     * Generate a null document which will be the "first" document for a given field.
     */
    static BSONObj generateNullDocument(const ESCTwiceDerivedTagToken& tagToken,
                                        const ESCTwiceDerivedValueToken& valueToken,
                                        uint64_t pos,
                                        uint64_t count);

    /**
     * Generate a insert ESC document.
     */
    static BSONObj generateInsertDocument(const ESCTwiceDerivedTagToken& tagToken,
                                          const ESCTwiceDerivedValueToken& valueToken,
                                          uint64_t index,
                                          uint64_t count);

    /**
     * Generate a compaction placeholder ESC document.
     */
    static BSONObj generateCompactionPlaceholderDocument(
        const ESCTwiceDerivedTagToken& tagToken,
        const ESCTwiceDerivedValueToken& valueToken,
        uint64_t index,
        uint64_t count);

    /**
     * Decrypt the null document.
     */
    static StatusWith<ESCNullDocument> decryptNullDocument(
        const ESCTwiceDerivedValueToken& valueToken, BSONObj& doc);

    /**
     * Decrypt the null document.
     */
    static StatusWith<ESCNullDocument> decryptNullDocument(
        const ESCTwiceDerivedValueToken& valueToken, BSONObj&& doc);

    /**
     * Search for the highest document id for a given field/value pair based on the token.
     */
    static boost::optional<uint64_t> emuBinary(const FLEStateCollectionReader& reader,
                                               const ESCTwiceDerivedTagToken& tagToken,
                                               const ESCTwiceDerivedValueToken& valueToken);

    // ===== Protocol Version 2 =====
    /**
     * Generate the _id value for a non-anchor record
     */
    static PrfBlock generateNonAnchorId(const ESCTwiceDerivedTagToken& tagToken, uint64_t cpos);

    /**
     * Generate a non-anchor ESC document for inserts.
     */
    static BSONObj generateNonAnchorDocument(const ESCTwiceDerivedTagToken& tagToken,
                                             uint64_t cpos);

    /**
     * Generate an anchor ESC document for compacts.
     */
    static BSONObj generateAnchorDocument(const ESCTwiceDerivedTagToken& tagToken,
                                          const ESCTwiceDerivedValueToken& valueToken,
                                          uint64_t apos,
                                          uint64_t cpos);

    /**
     * Generate a null anchor ESC document for cleanups.
     */
    static BSONObj generateNullAnchorDocument(const ESCTwiceDerivedTagToken& tagToken,
                                              const ESCTwiceDerivedValueToken& valueToken,
                                              uint64_t apos,
                                              uint64_t cpos);

    /*
     * Note on EmuBinaryV2 results:
     *    i = non-anchor position (cpos)
     *    x = anchor position (apos)
     *
     *    (i == 0) means no non-anchors AND no anchors exist at all. (implies x == 0).
     *    (i == null) means no new non-anchors since the last-recorded cpos in an anchor.
     *                Implies at least one anchor exists (x == null OR x > 0).
     *    (i > 0) means only non-anchors exist OR new non-anchors have been added since
     *            the last-recorded cpos in an anchor.
     *    (x == 0) means no anchors exist.
     *    (x == null) means a null anchor exists, and no new anchors since the apos in
     *                the null anchor.
     *    (x > 0) means non-null anchors exist without a null anchor OR new non-null anchors
     *            have been added since the last-recorded apos in the null anchor.
     */
    static EmuBinaryResult emuBinaryV2(const FLEStateCollectionReader& reader,
                                       const ESCTwiceDerivedTagToken& tagToken,
                                       const ESCTwiceDerivedValueToken& valueToken);
    static boost::optional<uint64_t> binaryHops(const FLEStateCollectionReader& reader,
                                                const ESCTwiceDerivedTagToken& tagToken,
                                                const ESCTwiceDerivedValueToken& valueToken,
                                                boost::optional<uint64_t> x,
                                                FLEStatusSection::EmuBinaryTracker& tracker);

    /**
     * Get the set of counts from ESC for a set of tags. Returns counts for these fields suitable
     * either for query or insert based on the type parameter.
     *
     * Returns a vector of zeros if the collection does not exist.
     */
    static std::vector<std::vector<FLEEdgeCountInfo>> getTags(
        const FLEStateCollectionReader& reader,
        const std::vector<std::vector<FLEEdgePrfBlock>>& tokensSets,
        FLETagQueryInterface::TagQueryType type);
};

/**
 * Specialization of ESCollectionCommon for AnchorPadding(Key|Value)Tokens
 * with a custom anchor padding document generator.
 */
class ESCCollectionAnchorPadding
    : public ESCCollectionCommon<AnchorPaddingKeyToken, AnchorPaddingValueToken> {
public:
    static PrfBlock generateNullAnchorId(const AnchorPaddingKeyToken& tagToken);
    static PrfBlock generateAnchorId(const AnchorPaddingKeyToken& tagToken, uint64_t apos);

    static BSONObj generateNullAnchorDocument(const AnchorPaddingKeyToken& keyToken,
                                              const AnchorPaddingValueToken& valueToken,
                                              uint64_t apos,
                                              uint64_t /* cpos ignored */);

    static BSONObj generatePaddingDocument(const AnchorPaddingKeyToken& keyToken,
                                           const AnchorPaddingValueToken& valueToken,
                                           uint64_t apos);
};

/**
 * Type safe abstraction over the key vault to support unit testing. Used by the various decryption
 * routines to retrieve the correct keys.
 *
 * Keys are identified by UUID in the key vault.
 */
class FLEKeyVault {
public:
    virtual ~FLEKeyVault();

    FLEUserKeyAndId getUserKeyById(const UUID& uuid) {
        return getKeyById<FLEKeyType::User>(uuid);
    }

    FLEIndexKeyAndId getIndexKeyById(const UUID& uuid) {
        return getKeyById<FLEKeyType::Index>(uuid);
    }

    /**
     * Return raw, encrypted keys from the key store
     */
    virtual BSONObj getEncryptedKey(const UUID& uuid) = 0;

    /**
     * Returns the local kms key that protects the raw keys
     */
    virtual SymmetricKey& getKMSLocalKey() = 0;

protected:
    virtual KeyMaterial getKey(const UUID& uuid) = 0;

private:
    template <FLEKeyType KeyT>
    FLEKeyAndId<KeyT> getKeyById(const UUID& uuid) {
        auto keyMaterial = getKey(uuid);
        return FLEKeyAndId<KeyT>(keyMaterial, uuid);
    }
};

using ContentionFactorFn = std::function<uint64_t(const FLE2EncryptionPlaceholder&)>;

class FLEClientCrypto {
public:
    static FLE2FindEqualityPayloadV2 serializeFindPayloadV2(FLEIndexKeyAndId indexKey,
                                                            FLEUserKeyAndId userKey,
                                                            BSONElement element,
                                                            uint64_t maxContentionFactor);

    static FLE2FindRangePayloadV2 serializeFindRangePayloadV2(FLEIndexKeyAndId indexKey,
                                                              FLEUserKeyAndId userKey,
                                                              const std::vector<std::string>& edges,
                                                              uint64_t maxContentionFactor,
                                                              uint32_t sparsity,
                                                              const FLE2RangeFindSpec& spec);

    static FLE2FindRangePayloadV2 serializeFindRangeStubV2(const FLE2RangeFindSpec& spec);

    /**
     * Generates a client-side payload that is sent to the server.
     *
     * Input is a document with FLE2EncryptionPlaceholder placeholders.
     *
     * For each field, transforms the field into BinData 6 with a prefix byte of 4
     *
     * {
     *   d : EDCDerivedFromDataTokenAndContentionFactorToken
     *   s : ESCDerivedFromDataTokenAndContentionFactorToken
     *   p : Encrypt(ECOCToken, ESCDerivedFromDataTokenAndContentionFactorToken) v :
     * Encrypt(K_KeyId, value), e : ServerDataEncryptionLevel1Token,
     * }
     */
    static BSONObj transformPlaceholders(const BSONObj& obj, FLEKeyVault* keyVault);

    /**
     * Generates a client-side payload that is sent to the server. Contention factor is given
     * explicitly as a lambda expression.
     */
    static BSONObj transformPlaceholders(const BSONObj& obj,
                                         FLEKeyVault* keyVault,
                                         const ContentionFactorFn& contentionFactor);


    /**
     * For every encrypted field path in the EncryptedFieldConfig, this generates
     * a compaction token derived from the field's index key, which is retrieved from
     * the supplied FLEKeyVault using the field's key ID.
     *
     * Returns a BSON object mapping the encrypted field path to its compaction token,
     * which is a general BinData value.
     */
    static BSONObj generateCompactionTokens(const EncryptedFieldConfig& cfg, FLEKeyVault* keyVault);

    /**
     * Decrypts a document. Only supports FLE2.
     */
    static BSONObj decryptDocument(BSONObj& doc, FLEKeyVault* keyVault);

    /**
     * Validate the tags array exists and is of the right type.
     */
    static void validateTagsArray(const BSONObj& doc);
};

/*
 * Values of ECOC documents
 *
 * Encrypt(ECOCToken, ESCDerivedFromDataTokenAndContentionFactorToken)
 *
 * struct {
 *    uint8_t[32] esc;
 * }
 */
struct EncryptedStateCollectionTokens {
public:
    EncryptedStateCollectionTokens(ESCDerivedFromDataTokenAndContentionFactorToken s) : esc(s) {}

    static StatusWith<EncryptedStateCollectionTokens> decryptAndParse(ECOCToken token,
                                                                      ConstDataRange cdr);
    StatusWith<std::vector<uint8_t>> serialize(ECOCToken token);

    ESCDerivedFromDataTokenAndContentionFactorToken esc;
};

struct ECOCCompactionDocumentV2 {

    static ECOCCompactionDocumentV2 parseAndDecrypt(const BSONObj& document,
                                                    const ECOCToken& token);

    bool operator==(const ECOCCompactionDocumentV2& other) const {
        return (fieldName == other.fieldName) && (esc == other.esc);
    }

    template <typename H>
    friend H AbslHashValue(H h, const ECOCCompactionDocumentV2& doc) {
        return H::combine(std::move(h), doc.fieldName, doc.esc);
    }

    bool isEquality() const {
        return isLeaf == boost::none;
    }

    bool isRange() const {
        return isLeaf != boost::none;
    }

    // Id is not included as it unimportant
    std::string fieldName;
    ESCDerivedFromDataTokenAndContentionFactorToken esc;
    boost::optional<bool> isLeaf;
    boost::optional<AnchorPaddingRootToken> anchorPaddingRootToken;
};

/**
 * Class to read/write the metadata block consisting of the encrypted counter
 * and contention factor, the tag, and the encrypted 128-bit string of zeros.
 *
 * In QE protocol version 2, this block appears exactly once in the on-disk
 * format of equality-indexed encrypted values, and at least once in the on-disk
 * format of range-indexed encrypted values.
 *
 * The metadata block serialization consists of the following:
 * struct {
 *   uint8_t[32] encryptedCountersBlob;
 *   uint8_t[32] tag;
 *   uint8_t[32] encryptedZerosBlob;
 * }
 *
 * Decryption of encryptedCountersBlob results in:
 * struct {
 *   uint64_t counter;
 *   uint64_t contentionFactor;
 * }
 *
 * Decryption of encryptedZerosBlob results in:
 * struct {
 *   uint8_t[16] zerosBlob;
 * }
 */
struct FLE2TagAndEncryptedMetadataBlock {
    using ZerosBlob = std::array<std::uint8_t, 16>;
    using EncryptedCountersBlob =
        std::array<std::uint8_t, sizeof(uint64_t) * 2 + crypto::aesCTRIVSize>;
    using EncryptedZerosBlob = std::array<std::uint8_t, sizeof(ZerosBlob) + crypto::aesCTRIVSize>;
    using SerializedBlob =
        std::array<std::uint8_t,
                   sizeof(EncryptedCountersBlob) + sizeof(PrfBlock) + sizeof(EncryptedZerosBlob)>;

    FLE2TagAndEncryptedMetadataBlock(uint64_t countParam,
                                     uint64_t contentionFactorParam,
                                     PrfBlock tagParam);
    FLE2TagAndEncryptedMetadataBlock(uint64_t countParam,
                                     uint64_t contentionFactorParam,
                                     PrfBlock tagParam,
                                     ZerosBlob zerosParam);

    StatusWith<std::vector<uint8_t>> serialize(ServerDerivedFromDataToken token);

    static StatusWith<FLE2TagAndEncryptedMetadataBlock> decryptAndParse(
        ServerDerivedFromDataToken token, ConstDataRange serializedBlock);

    static StatusWith<PrfBlock> parseTag(ConstDataRange serializedBlock);

    /*
     * Decrypts and returns only the zeros blob from the serialized
     * FLE2TagAndEncryptedMetadataBlock in serializedBlock.
     */
    static StatusWith<ZerosBlob> decryptZerosBlob(ServerZerosEncryptionToken token,
                                                  ConstDataRange serializedBlock);

    static bool isValidZerosBlob(const ZerosBlob& blob);

    uint64_t count;
    uint64_t contentionFactor;
    PrfBlock tag;
    ZerosBlob zeros;
};

/**
 * Class to read/write QE protocol version 2 of Equality Indexed
 * Encrypted Values.
 *
 * Fields are encrypted with the following:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 14;
 *   uint8_t key_uuid[16];
 *   uint8_t original_bson_type;
 *   ciphertext[ciphertext_length];
 *   metadataBlock;
 * }
 * where ciphertext computed as:
 *   Encrypt(ServerDataEncryptionLevel1Token, clientCiphertext)
 * and metadataBlock is a serialized FLE2TagAndEncryptedMetadataBlock.
 *
 * The specification needs to be in sync with the validation in 'bson_validate.cpp'.
 */
struct FLE2IndexedEqualityEncryptedValueV2 {
    FLE2IndexedEqualityEncryptedValueV2(const FLE2InsertUpdatePayloadV2& payload,
                                        PrfBlock tag,
                                        uint64_t counter);
    FLE2IndexedEqualityEncryptedValueV2(BSONType typeParam,
                                        UUID indexKeyIdParam,
                                        std::vector<uint8_t> clientEncryptedValueParam,
                                        FLE2TagAndEncryptedMetadataBlock metadataBlockParam);

    struct ParsedFields {
        UUID keyId;
        BSONType bsonType;
        ConstDataRange ciphertext;
        ConstDataRange metadataBlock;
    };
    static StatusWith<ParsedFields> parseAndValidateFields(ConstDataRange serializedServerValue);

    static StatusWith<std::vector<uint8_t>> parseAndDecryptCiphertext(
        ServerDataEncryptionLevel1Token serverEncryptionToken,
        ConstDataRange serializedServerValue);

    static StatusWith<FLE2TagAndEncryptedMetadataBlock> parseAndDecryptMetadataBlock(
        ServerDerivedFromDataToken serverDataDerivedToken, ConstDataRange serializedServerValue);

    static StatusWith<PrfBlock> parseMetadataBlockTag(ConstDataRange serializedServerValue);

    static StatusWith<UUID> readKeyId(ConstDataRange serializedServerValue);

    static StatusWith<BSONType> readBsonType(ConstDataRange serializedServerValue);

    StatusWith<std::vector<uint8_t>> serialize(
        ServerDataEncryptionLevel1Token serverEncryptionToken,
        ServerDerivedFromDataToken serverDataDerivedToken);

    BSONType bsonType;
    UUID indexKeyId;
    std::vector<uint8_t> clientEncryptedValue;
    FLE2TagAndEncryptedMetadataBlock metadataBlock;
};


/**
 * Class to read/write FLE2 Unindexed Encrypted Values (for protocol version 2)
 *
 * Fields are encrypted with the following:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 16;
 *   uint8_t key_uuid[16];
 *   uint8_t original_bson_type;
 *   ciphertext[ciphertext_length];
 * } blob;
 *
 * The specification needs to be in sync with the validation in 'bson_validate.cpp'.
 */
struct FLE2UnindexedEncryptedValueV2 {
    static std::vector<uint8_t> serialize(const FLEUserKeyAndId& userKey,
                                          const BSONElement& element);
    static std::pair<BSONType, std::vector<uint8_t>> deserialize(FLEKeyVault* keyVault,
                                                                 ConstDataRange blob);

    /*
     * The block cipher mode used with AES to encrypt/decrypt the value
     */
    static constexpr crypto::aesMode mode = crypto::aesMode::cbc;

    /*
     * The FLE type associated with this unindexed value
     */
    static constexpr EncryptedBinDataType fleType =
        EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2;

    /*
     * The size of the AAD used in AEAD encryption. The AAD consists of the fleType (1), the
     * key UUID (16), and the BSON type of the value (1).
     */
    static constexpr size_t assocDataSize = sizeof(uint8_t) + sizeof(UUID) + sizeof(uint8_t);
};

struct FLEEdgeToken {
    EDCDerivedFromDataTokenAndContentionFactorToken edc;
    ESCDerivedFromDataTokenAndContentionFactorToken esc;
};

/**
 * Class to read/write QE protocol version 2 of Range Indexed
 * Encrypted Values.
 *
 * Fields are encrypted with the following:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 15;
 *   uint8_t key_uuid[16];
 *   uint8_t original_bson_type;
 *   uint8_t edge_count;
 *   ciphertext[ciphertext_length];
 *   vector of metadataBlocks;
 * }
 * where ciphertext computed as:
 *   Encrypt(ServerDataEncryptionLevel1Token, clientCiphertext)
 * and metadataBlock is a vector of serialized FLE2TagAndEncryptedMetadataBlock.
 *
 * The specification needs to be in sync with the validation in 'bson_validate.cpp'.
 */
struct FLE2IndexedRangeEncryptedValueV2 {
    FLE2IndexedRangeEncryptedValueV2(const FLE2InsertUpdatePayloadV2& payload,
                                     std::vector<PrfBlock> tags,
                                     const std::vector<uint64_t>& counters);
    FLE2IndexedRangeEncryptedValueV2(
        BSONType typeParam,
        UUID indexKeyIdParam,
        std::vector<uint8_t> clientEncryptedValueParam,
        std::vector<FLE2TagAndEncryptedMetadataBlock> metadataBlockParam);

    struct ParsedFields {
        UUID keyId;
        BSONType bsonType;
        uint8_t edgeCount;
        ConstDataRange ciphertext;
        std::vector<ConstDataRange> metadataBlocks;
    };
    static StatusWith<ParsedFields> parseAndValidateFields(ConstDataRange serializedServerValue);

    static StatusWith<std::vector<uint8_t>> parseAndDecryptCiphertext(
        ServerDataEncryptionLevel1Token serverEncryptionToken,
        ConstDataRange serializedServerValue);

    static StatusWith<std::vector<FLE2TagAndEncryptedMetadataBlock>> parseAndDecryptMetadataBlocks(
        const std::vector<ServerDerivedFromDataToken>& serverDataDerivedTokens,
        ConstDataRange serializedServerValue);

    static StatusWith<std::vector<PrfBlock>> parseMetadataBlockTags(
        ConstDataRange serializedServerValue);

    static StatusWith<UUID> readKeyId(ConstDataRange serializedServerValue);

    static StatusWith<BSONType> readBsonType(ConstDataRange serializedServerValue);

    StatusWith<std::vector<uint8_t>> serialize(
        ServerDataEncryptionLevel1Token serverEncryptionToken,
        const std::vector<ServerDerivedFromDataToken>& serverDataDerivedTokens);

    BSONType bsonType;
    UUID indexKeyId;
    std::vector<uint8_t> clientEncryptedValue;
    std::vector<FLE2TagAndEncryptedMetadataBlock> metadataBlocks;
};

struct EDCServerPayloadInfo {
    static ESCDerivedFromDataTokenAndContentionFactorToken getESCToken(ConstDataRange cdr);

    bool isRangePayload() const {
        return payload.getEdgeTokenSet().has_value();
    }

    FLE2InsertUpdatePayloadV2 payload;
    std::string fieldPathName;
    std::vector<uint64_t> counts;
};

struct EDCIndexedFields {
    ConstDataRange value;

    std::string fieldPathName;
};

inline bool operator<(const EDCIndexedFields& left, const EDCIndexedFields& right) {
    if (left.fieldPathName == right.fieldPathName) {
        if (left.value.length() != right.value.length()) {
            return left.value.length() < right.value.length();
        }

        if (left.value.length() == 0 && right.value.length() == 0) {
            return false;
        }

        return memcmp(left.value.data(), right.value.data(), left.value.length()) < 0;
    }
    return left.fieldPathName < right.fieldPathName;
}

struct FLEDeleteToken {
    ECOCToken ecocToken;
    ServerDataEncryptionLevel1Token serverEncryptionToken;
};

/**
 * Manipulates the EDC collection.
 *
 * To finalize a document for insertion
 *
 * 1. Get all the encrypted fields that need counters via getEncryptedFieldInfo()
 * 2. Choose counters
 * 3. Finalize the insertion with finalizeForInsert().
 */
class EDCServerCollection {
public:
    /**
     * Validate that payload is compatible with schema
     */
    static void validateEncryptedFieldInfo(BSONObj& obj,
                                           const EncryptedFieldConfig& efc,
                                           bool bypassDocumentValidation);

    /**
     * Validates that the on-disk encrypted values in the input document are
     * compatible with the current QE protocol version.
     * Used during updates to verify that the modified document's pre-image can be
     * safely updated per the protocol compatibility rules.
     */
    static void validateModifiedDocumentCompatibility(BSONObj& obj);


    /**
     * Get information about all FLE2InsertUpdatePayload payloads
     */
    static std::vector<EDCServerPayloadInfo> getEncryptedFieldInfo(BSONObj& obj);

    /**
     * Generate a search tag
     *
     * HMAC(EDCTwiceDerivedToken, count)
     */
    static PrfBlock generateTag(EDCTwiceDerivedToken edcTwiceDerived, FLECounter count);
    static PrfBlock generateTag(const EDCServerPayloadInfo& payload);
    static PrfBlock generateTag(const FLEEdgeToken& token, FLECounter count);
    static std::vector<PrfBlock> generateTags(const EDCServerPayloadInfo& rangePayload);

    /**
     * Generate all the EDC tokens
     */
    static std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> generateEDCTokens(
        EDCDerivedFromDataToken token, uint64_t maxContentionFactor);
    static std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> generateEDCTokens(
        ConstDataRange rawToken, uint64_t maxContentionFactor);

    /**
     * Consumes a payload from a MongoDB client for insert.
     *
     * Converts FLE2InsertUpdatePayload to a final insert payload and updates __safeContent__ with
     * new tags.
     */
    static BSONObj finalizeForInsert(const BSONObj& doc,
                                     const std::vector<EDCServerPayloadInfo>& serverPayload);

    /**
     * Consumes a payload from a MongoDB client for update, modifier update style.
     *
     * Converts any FLE2InsertUpdatePayload found to the final insert payload. Adds or updates the
     * the existing $push to add __safeContent__ tags.
     */
    static BSONObj finalizeForUpdate(const BSONObj& doc,
                                     const std::vector<EDCServerPayloadInfo>& serverPayload);

    /**
     * Generate an update modifier document with $pull to remove stale tags.
     *
     * Generates:
     *
     * { $pull : {__safeContent__ : {$in : [tag..] } } }
     */
    static BSONObj generateUpdateToRemoveTags(const std::vector<PrfBlock>& tagsToPull);

    /**
     * Get a list of encrypted, indexed fields.
     */
    static std::vector<EDCIndexedFields> getEncryptedIndexedFields(BSONObj& obj);

    /**
     * Get a list of fields to remove.
     *
     * An update is performed in two steps:
     * 1. Perform the update of the encrypted fields
     *    - After step 1, the updated fields are correct, new tags have been added to
     *    __safeContent__ but the __safeContent__ still contains stale tags.
     * 2. Remove the old tags
     *
     * To do step 2, we need a list of removed tags. To do this we get a list of indexed encrypted
     * fields in both and subtract the fields in the newDocument from originalDocument. The
     * remaining fields are the ones we need to remove.
     */
    static std::vector<EDCIndexedFields> getRemovedFields(
        std::vector<EDCIndexedFields>& originalDocument,
        std::vector<EDCIndexedFields>& newDocument);

    /**
     * Generates the list of stale tags that need to be removed on an update.
     * This first calculates the set difference between the original document and
     * the new document using getRemovedFields(), then acquires the tags for each of the
     * fields left over. These are the tags that need to be removed from __safeContent__.
     *
     * This sorts the input vectors.
     */
    static std::vector<PrfBlock> getRemovedTags(std::vector<EDCIndexedFields>& originalDocument,
                                                std::vector<EDCIndexedFields>& newDocument);
};


class EncryptionInformationHelpers {
public:
    /**
     * Serialize EncryptedFieldConfig to a EncryptionInformation with
     * EncryptionInformation.schema = { nss: EncryptedFieldConfig}
     */
    static BSONObj encryptionInformationSerialize(const NamespaceString& nss,
                                                  const EncryptedFieldConfig& ef);
    static BSONObj encryptionInformationSerialize(const NamespaceString& nss,
                                                  const BSONObj& encryptedFields);
    /**
     * Get a schema from EncryptionInformation and ensure the esc/ecoc are setup correctly.
     */
    static EncryptedFieldConfig getAndValidateSchema(const NamespaceString& nss,
                                                     const EncryptionInformation& ei);
};

/**
 * A parsed element in the compaction tokens BSON object from
 * a compactStructuredEncryptionData command
 */
struct CompactionToken {
    std::string fieldPathName;
    ECOCToken token;
    boost::optional<AnchorPaddingRootToken> anchorPaddingToken;

    bool isEquality() const {
        return anchorPaddingToken == boost::none;
    }

    bool isRange() const {
        return anchorPaddingToken != boost::none;
    }
};

class CompactionHelpers {
public:
    /**
     * Converts the compaction tokens BSON object that contains encrypted
     * field paths as the key, and ECOC tokens as the value, to a list of
     * string and ECOCToken pairs.
     */
    static std::vector<CompactionToken> parseCompactionTokens(BSONObj compactionTokens);

    /**
     * Validates the compaction tokens BSON contains an element for each field
     * in the encrypted field config
     */
    static void validateCompactionTokens(const EncryptedFieldConfig& efc, BSONObj compactionTokens);

    /**
     * Validates the compaction tokens BSON contains an element for each field
     * in the encrypted field config
     */
    static void validateCleanupTokens(const EncryptedFieldConfig& efc, BSONObj cleanupTokens);

private:
    static void _validateTokens(const EncryptedFieldConfig& efc, BSONObj tokens, StringData cmd);
};

/**
 * Split a ConstDataRange into a byte for EncryptedBinDataType and a ConstDataRange for the trailing
 * bytes
 *
 * Verifies that EncryptedBinDataType is valid.
 */
std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedConstDataRange(ConstDataRange cdr);

struct ParsedFindEqualityPayload {
    ESCDerivedFromDataToken escToken;
    EDCDerivedFromDataToken edcToken;
    boost::optional<std::int64_t> maxCounter;

    // v2 fields
    ServerDerivedFromDataToken serverDataDerivedToken;

    explicit ParsedFindEqualityPayload(BSONElement fleFindPayload);
    explicit ParsedFindEqualityPayload(const Value& fleFindPayload);
    explicit ParsedFindEqualityPayload(ConstDataRange cdr);
};

struct FLEFindEdgeTokenSet {
    EDCDerivedFromDataToken edc;
    ESCDerivedFromDataToken esc;

    ServerDerivedFromDataToken server;
};

struct ParsedFindRangePayload {
    boost::optional<std::vector<FLEFindEdgeTokenSet>> edges;

    Fle2RangeOperator firstOp;
    boost::optional<Fle2RangeOperator> secondOp;
    std::int32_t payloadId{};

    std::int64_t maxCounter{};
    boost::optional<std::int32_t> sparsity{};
    boost::optional<std::int32_t> trimFactor{};
    boost::optional<std::int32_t> precision{};
    boost::optional<IDLAnyType> indexMin{};
    boost::optional<IDLAnyType> indexMax{};

    explicit ParsedFindRangePayload(BSONElement fleFindRangePayload);
    explicit ParsedFindRangePayload(const Value& fleFindRangePayload);
    explicit ParsedFindRangePayload(ConstDataRange cdr);

    bool isStub() {
        return !edges.has_value();
    }
};


/**
 * Edges calculator
 */

class Edges {
public:
    Edges(std::string leaf, int sparsity, const boost::optional<int>& trimFactor);
    std::vector<StringData> get();
    std::size_t size() const;
    const std::string& getLeaf() const {
        return _leaf;
    }

private:
    std::string _leaf;
    int _sparsity;
    int _trimFactor;
};

std::unique_ptr<Edges> getEdgesInt32(int32_t value,
                                     boost::optional<int32_t> min,
                                     boost::optional<int32_t> max,
                                     int sparsity,
                                     const boost::optional<int>& trimFactor);

std::unique_ptr<Edges> getEdgesInt64(int64_t value,
                                     boost::optional<int64_t> min,
                                     boost::optional<int64_t> max,
                                     int sparsity,
                                     const boost::optional<int>& trimFactor);

std::unique_ptr<Edges> getEdgesDouble(double value,
                                      boost::optional<double> min,
                                      boost::optional<double> max,
                                      boost::optional<uint32_t> precision,
                                      int sparsity,
                                      const boost::optional<int>& trimFactor);

std::unique_ptr<Edges> getEdgesDecimal128(Decimal128 value,
                                          boost::optional<Decimal128> min,
                                          boost::optional<Decimal128> max,
                                          boost::optional<uint32_t> precision,
                                          int sparsity,
                                          const boost::optional<int>& trimFactor);

// Equivalent to a full edges calculation without creating an intemediate vector.
// getEdgesT(min, min, max, precision, sparsity, trimFactor).size()
std::uint64_t getEdgesLength(BSONType fieldType, StringData fieldPath, QueryTypeConfig config);

/**
 * Mincover calculator
 */

std::vector<std::string> minCoverInt32(int32_t lowerBound,
                                       bool includeLowerBound,
                                       int32_t upperBound,
                                       bool includeUpperBound,
                                       boost::optional<int32_t> min,
                                       boost::optional<int32_t> max,
                                       int sparsity,
                                       const boost::optional<int>& trimFactor);

std::vector<std::string> minCoverInt64(int64_t lowerBound,
                                       bool includeLowerBound,
                                       int64_t upperBound,
                                       bool includeUpperBound,
                                       boost::optional<int64_t> min,
                                       boost::optional<int64_t> max,
                                       int sparsity,
                                       const boost::optional<int>& trimFactor);

std::vector<std::string> minCoverDouble(double lowerBound,
                                        bool includeLowerBound,
                                        double upperBound,
                                        bool includeUpperBound,
                                        boost::optional<double> min,
                                        boost::optional<double> max,
                                        boost::optional<uint32_t> precision,
                                        int sparsity,
                                        const boost::optional<int>& trimFactor);

std::vector<std::string> minCoverDecimal128(Decimal128 lowerBound,
                                            bool includeLowerBound,
                                            Decimal128 upperBound,
                                            bool includeUpperBound,
                                            boost::optional<Decimal128> min,
                                            boost::optional<Decimal128> max,
                                            boost::optional<uint32_t> precision,
                                            int sparsity,
                                            const boost::optional<int>& trimFactor);

class FLEUtil {
public:
    static std::vector<uint8_t> vectorFromCDR(ConstDataRange cdr);
    static PrfBlock blockToArray(const SHA256Block& block);

    /**
     * Compute HMAC-SHA-256
     */
    static PrfBlock prf(ConstDataRange key, ConstDataRange cdr);

    static PrfBlock prf(ConstDataRange key, uint64_t value);

    /**
     * Decrypt AES-256-CTR encrypted data. Exposed for benchmarking purposes.
     */
    static StatusWith<std::vector<uint8_t>> decryptData(ConstDataRange key,
                                                        ConstDataRange cipherText);
};

/**
 * Utility functions manipulating buffers.
 */
PrfBlock PrfBlockfromCDR(const ConstDataRange& block);

ConstDataRange binDataToCDR(BSONElement element);

template <typename T>
T parseFromCDR(ConstDataRange cdr) {
    ConstDataRangeCursor cdc(cdr);
    auto obj = cdc.readAndAdvance<Validated<BSONObj>>();

    IDLParserContext ctx("root");
    return T::parse(ctx, obj);
}

std::vector<uint8_t> toEncryptedVector(EncryptedBinDataType dt, const PrfBlock& block);

BSONBinData toBSONBinData(const std::vector<uint8_t>& buf);

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedBinData(const Value& value);

boost::optional<EncryptedBinDataType> getEncryptedBinDataType(const Value& value);
boost::optional<EncryptedBinDataType> getEncryptedBinDataType(const BSONElement& elt);

bool hasQueryType(const EncryptedField& field, QueryTypeEnum queryType);
bool hasQueryType(const EncryptedFieldConfig& config, QueryTypeEnum queryType);

QueryTypeConfig getQueryType(const EncryptedField& field, QueryTypeEnum queryType);

/**
 * Get the set of edges that minimally cover a range query specified by the given range spec and
 * sparsity
 */
std::vector<std::string> getMinCover(const FLE2RangeFindSpec& spec, uint8_t sparsity);
}  // namespace mongo
