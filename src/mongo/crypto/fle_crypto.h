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
#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/uuid.h"

namespace mongo {

constexpr auto kSafeContent = "__safeContent__";

using PrfBlock = std::array<std::uint8_t, 32>;
using KeyMaterial = SecureVector<std::uint8_t>;

// u = [1, max parallel clients)
using FLEContentionFactor = std::uint64_t;
using FLECounter = std::uint64_t;

/**
 * There are two types of keys that are user supplied.
 * 1. Index, aka S - this encrypts the index structures
 * 2. User, aka K - this encrypts the user data.
 *
 * These keys only exist on the client, they are never on the server-side.
 */
enum class FLEKeyType {
    Index,  // i.e. S
    User,   // i.e. K
};

/**
 * Template class to ensure unique C++ types for each key.
 */
template <FLEKeyType KeyT>
struct FLEKey {
    FLEKey() = default;

    FLEKey(KeyMaterial dataIn) : data(std::move(dataIn)) {
        // This is not a mistake; same keys will be used in FLE2 as in FLE1
        uassert(6364500,
                str::stream() << "Length of KeyMaterial is expected to be "
                              << crypto::kFieldLevelEncryptionKeySize << " bytes, found "
                              << data->size(),
                data->size() == crypto::kFieldLevelEncryptionKeySize);
    }

    ConstDataRange toCDR() const {
        return ConstDataRange(data->data(), data->data() + data->size());
    }

    // Actual type of the key
    FLEKeyType type{KeyT};

    // Raw bytes of the key
    KeyMaterial data;
};

using FLEIndexKey = FLEKey<FLEKeyType::Index>;
using FLEUserKey = FLEKey<FLEKeyType::User>;

/**
 * Key Material and its UUID id.
 *
 * The UUID is persisted into the serialized structures so that decryption is self-describing.
 */
template <FLEKeyType KeyT>
struct FLEKeyAndId {

    FLEKeyAndId(KeyMaterial material, UUID uuid) : key(material), keyId(uuid) {}

    FLEKey<KeyT> key;
    UUID keyId;
};

using FLEIndexKeyAndId = FLEKeyAndId<FLEKeyType::Index>;
using FLEUserKeyAndId = FLEKeyAndId<FLEKeyType::User>;


/*
 * The many token types are derived from the index key
 *
 * Terminology
 * f = field
 * v = value
 * u =
 *   - For non-contentious fields, we select the partition number, u, to be equal to 0.
 *   - For contentious fields, with a contention factor, p, we pick the partition number, u,
 * uniformly at random from the set {0, ..., p}.
 *
 * CollectionsLevel1Token = HMAC(IndexKey, 1) = K_{f,1}
 * ServerDataEncryptionLevel1Token = HMAC(IndexKey, 3) = K_{f,3} = Fs[f,3]
 *
 * EDCToken = HMAC(CollectionsLevel1Token, 1) = K^{edc}_f
 * ESCToken = HMAC(CollectionsLevel1Token, 2) = K^{esc}_f
 * ECCToken = HMAC(CollectionsLevel1Token, 3) = K^{ecc}_f
 * ECOCToken = HMAC(CollectionsLevel1Token, 4) = K^{ecoc}_f = Fs[f,1,4]
 *
 * EDCDerivedFromDataToken = HMAC(EDCToken, v) = K^{edc}_{f,v} = Fs[f,1,1,v]
 * ESCDerivedFromDataToken = HMAC(ESCToken, v) = K^{esc}_{f,v} = Fs[f,1,2,v]
 * ECCDerivedFromDataToken = HMAC(ECCToken, v) = K^{ecc}_{f,v} = Fs[f,1,3,v]
 *
 * EDCDerivedFromDataTokenAndContentionFactorToken = HMAC(EDCDerivedFromDataToken, u) =
 * Fs[f,1,1,v,u] ESCDerivedFromDataTokenAndContentionFactorToken = HMAC(ESCDerivedFromDataToken, u)
 * = Fs[f,1,2,v,u] ECCDerivedFromDataTokenAndContentionFactorToken = HMAC(ECCDerivedFromDataToken,
 * u) = Fs[f,1,3,v,u]
 *
 * EDCTwiceDerivedToken = HMAC(EDCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_edc(1)
 * ESCTwiceDerivedTagToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_esc(1)
 * ESCTwiceDerivedValueToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 2) = Fs_esc(2)
 * ECCTwiceDerivedTagToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_ecc(1)
 * ECCTwiceDerivedValueToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 2) = Fs_ecc(2)
 */
enum class FLETokenType {
    CollectionsLevel1Token,
    ServerDataEncryptionLevel1Token,

    EDCToken,
    ESCToken,
    ECCToken,
    ECOCToken,

    EDCDerivedFromDataToken,
    ESCDerivedFromDataToken,
    ECCDerivedFromDataToken,

    EDCDerivedFromDataTokenAndContentionFactorToken,
    ESCDerivedFromDataTokenAndContentionFactorToken,
    ECCDerivedFromDataTokenAndContentionFactorToken,

    EDCTwiceDerivedToken,
    ESCTwiceDerivedTagToken,
    ESCTwiceDerivedValueToken,
    ECCTwiceDerivedTagToken,
    ECCTwiceDerivedValueToken,

};

/**
 * Templated C++ class that contains a token. A templated class is used to create a strongly typed
 * API that is hard to misuse.
 */
template <FLETokenType TokenT>
struct FLEToken {
    FLEToken() = default;

    FLEToken(PrfBlock dataIn) : data(std::move(dataIn)) {}

    ConstDataRange toCDR() const {
        return ConstDataRange(data.data(), data.data() + data.size());
    }

    bool operator==(const FLEToken<TokenT>& other) const {
        return (type == other.type) && (data == other.data);
    }

    bool operator!=(const FLEToken<TokenT>& other) const {
        return !(*this == other);
    }

    template <typename H>
    friend H AbslHashValue(H h, const FLEToken<TokenT>& token) {
        return H::combine(std::move(h), token.type, token.data);
    }

    FLETokenType type{TokenT};
    PrfBlock data;
};


using CollectionsLevel1Token = FLEToken<FLETokenType::CollectionsLevel1Token>;
using ServerDataEncryptionLevel1Token = FLEToken<FLETokenType::ServerDataEncryptionLevel1Token>;
using EDCToken = FLEToken<FLETokenType::EDCToken>;
using ESCToken = FLEToken<FLETokenType::ESCToken>;
using ECCToken = FLEToken<FLETokenType::ECCToken>;
using ECOCToken = FLEToken<FLETokenType::ECOCToken>;
using EDCDerivedFromDataToken = FLEToken<FLETokenType::EDCDerivedFromDataToken>;
using ESCDerivedFromDataToken = FLEToken<FLETokenType::ESCDerivedFromDataToken>;
using ECCDerivedFromDataToken = FLEToken<FLETokenType::ECCDerivedFromDataToken>;
using EDCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>;
using ESCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::ESCDerivedFromDataTokenAndContentionFactorToken>;
using ECCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::ECCDerivedFromDataTokenAndContentionFactorToken>;
using EDCTwiceDerivedToken = FLEToken<FLETokenType::EDCTwiceDerivedToken>;
using ESCTwiceDerivedTagToken = FLEToken<FLETokenType::ESCTwiceDerivedTagToken>;
using ESCTwiceDerivedValueToken = FLEToken<FLETokenType::ESCTwiceDerivedValueToken>;
using ECCTwiceDerivedTagToken = FLEToken<FLETokenType::ECCTwiceDerivedTagToken>;
using ECCTwiceDerivedValueToken = FLEToken<FLETokenType::ECCTwiceDerivedValueToken>;


/**
 * Generate tokens from the Index Key
 */
class FLELevel1TokenGenerator {
public:
    /**
     * CollectionsLevel1Token = HMAC(IndexKey, 1) = K_{f,1}
     */
    static CollectionsLevel1Token generateCollectionsLevel1Token(FLEIndexKey indexKey);

    /**
     * CollectionsLevel1Token =HMAC(IndexKey, 3) = K_{f,3}
     */
    static ServerDataEncryptionLevel1Token generateServerDataEncryptionLevel1Token(
        FLEIndexKey indexKey);
};

/**
 * Generate tokens from the CollectionsLevel1Token for use with the various collections.
 */
class FLECollectionTokenGenerator {
public:
    /**
     * EDCToken = HMAC(CollectionsLevel1Token, 1) = K^{edc}_f
     */
    static EDCToken generateEDCToken(CollectionsLevel1Token token);

    /**
     * ESCToken = HMAC(CollectionsLevel1Token, 2) = K^{esc}_f
     */
    static ESCToken generateESCToken(CollectionsLevel1Token token);

    /**
     * ECCToken = HMAC(CollectionsLevel1Token, 3) = K^{ecc}_f
     */
    static ECCToken generateECCToken(CollectionsLevel1Token token);

    /**
     * ECOCToken = HMAC(CollectionsLevel1Token, 4) = K^{ecoc}_f
     */
    static ECOCToken generateECOCToken(CollectionsLevel1Token token);
};


/**
 * Generate tokens for the various collections derived from the user data.
 */
class FLEDerivedFromDataTokenGenerator {
public:
    /**
     * EDCDerivedFromDataToken = HMAC(EDCToken, v) = K^{edc}_{f,v}
     */
    static EDCDerivedFromDataToken generateEDCDerivedFromDataToken(EDCToken token,
                                                                   ConstDataRange value);

    /**
     * ESCDerivedFromDataToken = HMAC(ESCToken, v) = K^{esc}_{f,v}
     */
    static ESCDerivedFromDataToken generateESCDerivedFromDataToken(ESCToken token,
                                                                   ConstDataRange value);

    /**
     * ECCDerivedFromDataToken = HMAC(ECCToken, v) = K^{ecc}_{f,v}
     */
    static ECCDerivedFromDataToken generateECCDerivedFromDataToken(ECCToken token,
                                                                   ConstDataRange value);
};

/**
 * Generate tokens for the various collections derived from the user data and a contention factor.
 */
class FLEDerivedFromDataTokenAndContentionFactorTokenGenerator {
public:
    /**
     * EDCDerivedFromDataTokenAndContentionFactorToken = HMAC(EDCDerivedFromDataToken, u)
     */
    static EDCDerivedFromDataTokenAndContentionFactorToken
    generateEDCDerivedFromDataTokenAndContentionFactorToken(EDCDerivedFromDataToken token,
                                                            FLECounter counter);

    /**
     * ESCDerivedFromDataTokenAndContentionFactorToken = HMAC(ESCDerivedFromDataToken, u)
     */
    static ESCDerivedFromDataTokenAndContentionFactorToken
    generateESCDerivedFromDataTokenAndContentionFactorToken(ESCDerivedFromDataToken token,
                                                            FLECounter counter);

    /**
     * ECCDerivedFromDataTokenAndContentionFactorToken = HMAC(ECCDerivedFromDataToken, u)
     */
    static ECCDerivedFromDataTokenAndContentionFactorToken
    generateECCDerivedFromDataTokenAndContentionFactorToken(ECCDerivedFromDataToken token,
                                                            FLECounter counter);
};

/**
 * Generate tokens for the various collections derived from counter tokens.
 */
class FLETwiceDerivedTokenGenerator {
public:
    /**
     * EDCTwiceDerivedToken = HMAC(EDCDerivedFromDataTokenAndContentionFactorToken, 1)
     */
    static EDCTwiceDerivedToken generateEDCTwiceDerivedToken(
        EDCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ESCTwiceDerivedTagToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 1)
     */
    static ESCTwiceDerivedTagToken generateESCTwiceDerivedTagToken(
        ESCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ESCTwiceDerivedValueToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 2)
     */
    static ESCTwiceDerivedValueToken generateESCTwiceDerivedValueToken(
        ESCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ECCTwiceDerivedTagToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 1)
     */
    static ECCTwiceDerivedTagToken generateECCTwiceDerivedTagToken(
        ECCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ECCTwiceDerivedValueToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 2)
     */
    static ECCTwiceDerivedValueToken generateECCTwiceDerivedValueToken(
        ECCDerivedFromDataTokenAndContentionFactorToken token);
};


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
};

class ESCCollection {
public:
    /**
     * Generate the _id value
     */
    static PrfBlock generateId(ESCTwiceDerivedTagToken tagToken, boost::optional<uint64_t> index);

    /**
     * Generate a null document which will be the "first" document for a given field.
     */
    static BSONObj generateNullDocument(ESCTwiceDerivedTagToken tagToken,
                                        ESCTwiceDerivedValueToken valueToken,
                                        uint64_t pos,
                                        uint64_t count);

    /**
     * Generate a insert ESC document.
     */
    static BSONObj generateInsertDocument(ESCTwiceDerivedTagToken tagToken,
                                          ESCTwiceDerivedValueToken valueToken,
                                          uint64_t index,
                                          uint64_t count);

    /**
     * Generate a compaction placeholder ESC document.
     */
    static BSONObj generateCompactionPlaceholderDocument(ESCTwiceDerivedTagToken tagToken,
                                                         ESCTwiceDerivedValueToken valueToken,
                                                         uint64_t index,
                                                         uint64_t count);

    /**
     * Decrypt the null document.
     */
    static StatusWith<ESCNullDocument> decryptNullDocument(ESCTwiceDerivedValueToken valueToken,
                                                           BSONObj& doc);

    /**
     * Decrypt the null document.
     */
    static StatusWith<ESCNullDocument> decryptNullDocument(ESCTwiceDerivedValueToken valueToken,
                                                           BSONObj&& doc);

    /**
     * Decrypt a regular document.
     */
    static StatusWith<ESCDocument> decryptDocument(ESCTwiceDerivedValueToken valueToken,
                                                   BSONObj& doc);

    /**
     * Decrypt a regular document.
     */
    static StatusWith<ESCDocument> decryptDocument(ESCTwiceDerivedValueToken valueToken,
                                                   BSONObj&& doc);

    /**
     * Search for the highest document id for a given field/value pair based on the token.
     */
    static boost::optional<uint64_t> emuBinary(const FLEStateCollectionReader& reader,
                                               ESCTwiceDerivedTagToken tagToken,
                                               ESCTwiceDerivedValueToken valueToken);
};


/**
 * ECC Collection
 * - a record of deleted documents
 *
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, type || pos )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  (count || count) OR (start || end))
 * }
 *
 * where
 *  type = uint64_t
 *  pos = uint64_t
 *  value is either:
 *       count, count = uint64_t  // Null records
 *    OR
 *       start = uint64_t  // Other records
 *       end = uint64_t
 *
 * where type:
 *   0 - null record
 *   1 - regular record or compaction record
 *
 * where start and end:
 *   [1..UINT_64_MAX) - regular start and end
 *   UINT64_MAX - compaction placeholder
 *
 * Record types:
 *
 * Document Counts
 * Null: 0 or 1
 * Regular: 0 or more
 * Compaction: 0 or 1
 *
 * Null record:
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, null )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  count || count)
 * }
 *
 * Regular record:
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  start || end)
 * }
 *
 * Compaction placeholder record:
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  UINT64_MAX || UINT64_MAX)
 * }
 *
 * PlainText of tag
 * struct {
 *    uint64_t type;
 *    uint64_t pos;
 * }
 *
 * PlainText of value for null records
 * struct {
 *    uint64_t count;
 *    uint64_t ignored;
 * }
 *
 * PlainText of value for non-null records
 * struct {
 *    uint64_t start;
 *    uint64_t end;
 * }
 */
enum class ECCValueType : uint64_t {
    kNormal = 0,
    kCompactionPlaceholder = 1,
};


struct ECCNullDocument {
    // Id is not included as it HMAC generated and cannot be reversed
    uint64_t position;
};


struct ECCDocument {
    // Id is not included as it HMAC generated and cannot be reversed
    ECCValueType valueType;
    uint64_t start;
    uint64_t end;
};

inline bool operator==(const ECCDocument& left, const ECCDocument& right) {
    return (left.valueType == right.valueType && left.start == right.start &&
            left.end == right.end);
}

inline bool operator<(const ECCDocument& left, const ECCDocument& right) {
    if (left.start == right.start) {
        if (left.end == right.end) {
            return left.valueType < right.valueType;
        }
        return left.end < right.end;
    }
    return left.start < right.start;
}

class ECCCollection {
public:
    /**
     * Generate the _id value
     */
    static PrfBlock generateId(ECCTwiceDerivedTagToken tagToken, boost::optional<uint64_t> index);

    /**
     * Generate a null document which will be the "first" document for a given field.
     */
    static BSONObj generateNullDocument(ECCTwiceDerivedTagToken tagToken,
                                        ECCTwiceDerivedValueToken valueToken,
                                        uint64_t count);

    /**
     * Generate a regular ECC document for (count).
     *
     * Note: it is stored as (count, count)
     */
    static BSONObj generateDocument(ECCTwiceDerivedTagToken tagToken,
                                    ECCTwiceDerivedValueToken valueToken,
                                    uint64_t index,
                                    uint64_t count);

    /**
     * Generate a regular ECC document for (start, end)
     */
    static BSONObj generateDocument(ECCTwiceDerivedTagToken tagToken,
                                    ECCTwiceDerivedValueToken valueToken,
                                    uint64_t index,
                                    uint64_t start,
                                    uint64_t end);

    /**
     * Generate a compaction ECC document.
     */
    static BSONObj generateCompactionDocument(ECCTwiceDerivedTagToken tagToken,
                                              ECCTwiceDerivedValueToken valueToken,
                                              uint64_t index);

    /**
     * Decrypt the null document.
     */
    static StatusWith<ECCNullDocument> decryptNullDocument(ECCTwiceDerivedValueToken valueToken,
                                                           const BSONObj& doc);

    /**
     * Decrypt a regular document.
     */
    static StatusWith<ECCDocument> decryptDocument(ECCTwiceDerivedValueToken valueToken,
                                                   const BSONObj& doc);

    /**
     * Search for the highest document id for a given field/value pair based on the token.
     */
    static boost::optional<uint64_t> emuBinary(const FLEStateCollectionReader& reader,
                                               ECCTwiceDerivedTagToken tagToken,
                                               ECCTwiceDerivedValueToken valueToken);
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
    /**
     * Explicit encrypt a single value into a placeholder.
     *
     * Returns FLE2InsertUpdate payload
     */
    static std::vector<uint8_t> encrypt(BSONElement element,
                                        FLEIndexKeyAndId indexKey,
                                        FLEUserKeyAndId userKey,
                                        FLECounter counter);


    /**
     * Explicit decrypt a single value into type and value
     *
     * Supports decrypting FLE2IndexedEqualityEncryptedValue
     */
    static std::pair<BSONType, std::vector<uint8_t>> decrypt(ConstDataRange cdr,
                                                             FLEKeyVault* keyVault);

    static std::pair<BSONType, std::vector<uint8_t>> decrypt(BSONElement element,
                                                             FLEKeyVault* keyVault);

    static FLE2FindEqualityPayload parseFindPayload(ConstDataRange cdr);

    static FLE2FindEqualityPayload serializeFindPayload(FLEIndexKeyAndId indexKey,
                                                        FLEUserKeyAndId userKey,
                                                        BSONElement element,
                                                        uint64_t maxContentionFactor);

    static FLE2FindRangePayload serializeFindRangePayload(FLEIndexKeyAndId indexKey,
                                                          FLEUserKeyAndId userKey,
                                                          const std::vector<std::string>& edges,
                                                          uint64_t maxContentionFactor);

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
     *   c : ECCDerivedFromDataTokenAndContentionFactorToken
     *   p : Encrypt(ECOCToken, ESCDerivedFromDataTokenAndContentionFactorToken ||
     * ECCDerivedFromDataTokenAndContentionFactorToken) v : Encrypt(K_KeyId, value),
     *   e : ServerDataEncryptionLevel1Token,
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

    /**
     * Validate document
     *
     * Checks performed
     * 1. Fields, if present, are indexed the way specified
     * 2. All fields can be decrypted successfully
     * 3. There is a tag for each field and no extra tags
     */
    static void validateDocument(const BSONObj& doc,
                                 const EncryptedFieldConfig& efc,
                                 FLEKeyVault* keyVault);
};

/*
 * Values of ECOC documents
 *
 * Encrypt(ECOCToken, ESCDerivedFromDataTokenAndContentionFactorToken ||
 * ECCDerivedFromDataTokenAndContentionFactorToken)
 *
 * struct {
 *    uint8_t[32] esc;
 *    uint8_t[32] ecc;
 * }
 */
struct EncryptedStateCollectionTokens {
public:
    EncryptedStateCollectionTokens(ESCDerivedFromDataTokenAndContentionFactorToken s,
                                   ECCDerivedFromDataTokenAndContentionFactorToken c)
        : esc(s), ecc(c) {}

    static StatusWith<EncryptedStateCollectionTokens> decryptAndParse(ECOCToken token,
                                                                      ConstDataRange cdr);
    StatusWith<std::vector<uint8_t>> serialize(ECOCToken token);

    ESCDerivedFromDataTokenAndContentionFactorToken esc;
    ECCDerivedFromDataTokenAndContentionFactorToken ecc;
};


struct ECOCCompactionDocument {

    bool operator==(const ECOCCompactionDocument& other) const {
        return (fieldName == other.fieldName) && (esc == other.esc) && (ecc == other.ecc);
    }

    template <typename H>
    friend H AbslHashValue(H h, const ECOCCompactionDocument& doc) {
        return H::combine(std::move(h), doc.fieldName, doc.esc, doc.ecc);
    }

    // Id is not included as it unimportant
    std::string fieldName;
    ESCDerivedFromDataTokenAndContentionFactorToken esc;
    ECCDerivedFromDataTokenAndContentionFactorToken ecc;
};

/**
 * ECOC Collection schema
 * {
 *    _id : ObjectId() -- omitted so MongoDB can auto choose it
 *    fieldName : String,S
 *    value : Encrypt(ECOCToken, ESCDerivedFromDataTokenAndContentionFactorToken ||
 * ECCDerivedFromDataTokenAndContentionFactorToken)
 * }
 *
 * where
 *  value comes from client, see EncryptedStateCollectionTokens or
 * FLE2InsertUpdatePayload.getEncryptedTokens()
 *
 * Note:
 * - ECOC is a set of documents, they are unordered
 */
class ECOCCollection {
public:
    static BSONObj generateDocument(StringData fieldName, ConstDataRange payload);

    static ECOCCompactionDocument parseAndDecrypt(const BSONObj& doc, ECOCToken token);
};


/**
 * Class to read/write FLE2 Equality Indexed Encrypted Values
 *
 * Fields are encrypted with the following:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 7;
 *   uint8_t key_uuid[16];
 *   uint8  original_bson_type;
 *   ciphertext[ciphertext_length];
 * }
 *
 * Encrypt(ServerDataEncryptionLevel1Token, Struct(K_KeyId, v, count, d, s, c))
 *
 * struct {
 *   uint64_t length;
 *   uint8_t[length] cipherText; // UserKeyId + Encrypt(K_KeyId, value),
 *   uint64_t counter;
 *   uint8_t[32] edc;  // EDCDerivedFromDataTokenAndContentionFactorToken
 *   uint8_t[32] esc;  // ESCDerivedFromDataTokenAndContentionFactorToken
 *   uint8_t[32] ecc;  // ECCDerivedFromDataTokenAndContentionFactorToken
 *}
 *
 * The specification needs to be in sync with the validation in 'bson_validate.cpp'.
 */
struct FLE2IndexedEqualityEncryptedValue {
    FLE2IndexedEqualityEncryptedValue(FLE2InsertUpdatePayload payload, uint64_t counter);

    FLE2IndexedEqualityEncryptedValue(EDCDerivedFromDataTokenAndContentionFactorToken edcParam,
                                      ESCDerivedFromDataTokenAndContentionFactorToken escParam,
                                      ECCDerivedFromDataTokenAndContentionFactorToken eccParam,
                                      uint64_t countParam,
                                      BSONType typeParam,
                                      UUID indexKeyIdParam,
                                      std::vector<uint8_t> serializedServerValueParam);

    static StatusWith<FLE2IndexedEqualityEncryptedValue> decryptAndParse(
        ServerDataEncryptionLevel1Token token, ConstDataRange serializedServerValue);

    /**
     * Read the key id from the payload.
     */
    static StatusWith<UUID> readKeyId(ConstDataRange serializedServerValue);

    StatusWith<std::vector<uint8_t>> serialize(ServerDataEncryptionLevel1Token token);

    EDCDerivedFromDataTokenAndContentionFactorToken edc;
    ESCDerivedFromDataTokenAndContentionFactorToken esc;
    ECCDerivedFromDataTokenAndContentionFactorToken ecc;
    uint64_t count;
    BSONType bsonType;
    UUID indexKeyId;
    std::vector<uint8_t> clientEncryptedValue;
};

/**
 * Class to read/write FLE2 Unindexed Encrypted Values
 *
 * Fields are encrypted with the following:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 6;
 *   uint8_t key_uuid[16];
 *   uint8  original_bson_type;
 *   ciphertext[ciphertext_length];
 * } blob;
 *
 * The specification needs to be in sync with the validation in 'bson_validate.cpp'.
 */
struct FLE2UnindexedEncryptedValue {
    static std::vector<uint8_t> serialize(const FLEUserKeyAndId& userKey,
                                          const BSONElement& element);
    static std::pair<BSONType, std::vector<uint8_t>> deserialize(FLEKeyVault* keyVault,
                                                                 ConstDataRange blob);

    static constexpr size_t assocDataSize = sizeof(uint8_t) + sizeof(UUID) + sizeof(uint8_t);
};

struct FLEEdgeToken {
    EDCDerivedFromDataTokenAndContentionFactorToken edc;
    ESCDerivedFromDataTokenAndContentionFactorToken esc;
    ECCDerivedFromDataTokenAndContentionFactorToken ecc;
};

/**
 * Class to read/write FLE2 Range Indexed Encrypted Values
 *
 * Fields are encrypted with the following:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 9;
 *   uint8_t key_uuid[16];
 *   uint8  original_bson_type;
 *   ciphertext[ciphertext_length];
 * }
 *
 * Encrypt(ServerDataEncryptionLevel1Token, Struct(K_KeyId, v, edgeCount, [count, d, s, c] x
 *edgeCount ))
 *
 * struct {
 *   uint64_t length;
 *   uint8_t[length] cipherText; // UserKeyId + Encrypt(K_KeyId, value),
 *   uint32_t edgeCount;
 *   struct {
 *      uint64_t counter;
 *      uint8_t[32] edc;  // EDCDerivedFromDataTokenAndContentionFactorToken
 *      uint8_t[32] esc;  // ESCDerivedFromDataTokenAndContentionFactorToken
 *      uint8_t[32] ecc;  // ECCDerivedFromDataTokenAndContentionFactorToken
 *   } edges[edgeCount];
 *}
 *
 * The specification needs to be in sync with the validation in 'bson_validate.cpp'.
 */
struct FLE2IndexedRangeEncryptedValue {
    FLE2IndexedRangeEncryptedValue(FLE2InsertUpdatePayload payload,
                                   std::vector<uint64_t> countersParam);

    FLE2IndexedRangeEncryptedValue(std::vector<FLEEdgeToken> tokens,
                                   std::vector<uint64_t> countersParam,
                                   BSONType typeParam,
                                   UUID indexKeyIdParam,
                                   std::vector<uint8_t> serializedServerValueParam);

    static StatusWith<FLE2IndexedRangeEncryptedValue> decryptAndParse(
        ServerDataEncryptionLevel1Token token, ConstDataRange serializedServerValue);

    /**
     * Read the key id from the payload.
     */
    static StatusWith<UUID> readKeyId(ConstDataRange serializedServerValue);

    StatusWith<std::vector<uint8_t>> serialize(ServerDataEncryptionLevel1Token token);

    std::vector<FLEEdgeToken> tokens;
    std::vector<uint64_t> counters;
    BSONType bsonType;
    UUID indexKeyId;
    std::vector<uint8_t> clientEncryptedValue;
};

struct EDCServerPayloadInfo {
    static ESCDerivedFromDataTokenAndContentionFactorToken getESCToken(ConstDataRange cdr);

    FLE2InsertUpdatePayload payload;
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
     * Get information about all FLE2InsertUpdatePayload payloads
     */
    static std::vector<EDCServerPayloadInfo> getEncryptedFieldInfo(BSONObj& obj);

    static StatusWith<FLE2IndexedEqualityEncryptedValue> decryptAndParse(
        ServerDataEncryptionLevel1Token token, ConstDataRange serializedServerValue);

    static StatusWith<FLE2IndexedEqualityEncryptedValue> decryptAndParse(
        ConstDataRange token, ConstDataRange serializedServerValue);

    static StatusWith<FLE2IndexedRangeEncryptedValue> decryptAndParseRange(
        ConstDataRange token, ConstDataRange serializedServerValue);

    /**
     * Generate a search tag
     *
     * HMAC(EDCTwiceDerivedToken, count)
     */
    static PrfBlock generateTag(EDCTwiceDerivedToken edcTwiceDerived, FLECounter count);
    static PrfBlock generateTag(const EDCServerPayloadInfo& payload);
    static PrfBlock generateTag(const FLE2IndexedEqualityEncryptedValue& indexedValue);
    static PrfBlock generateTag(const FLEEdgeToken& token, FLECounter count);
    static std::vector<PrfBlock> generateTags(const FLE2IndexedRangeEncryptedValue& indexedValue);

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
    static BSONObj generateUpdateToRemoveTags(const std::vector<EDCIndexedFields>& removedFields,
                                              const StringMap<FLEDeleteToken>& tokenMap);

    /**
     * Get a list of encrypted, indexed fields.
     */
    static std::vector<EDCIndexedFields> getEncryptedIndexedFields(BSONObj& obj);

    /**
     * Get a list of tags to remove and add.
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
    static std::vector<EDCIndexedFields> getRemovedTags(
        std::vector<EDCIndexedFields>& originalDocument,
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
     * Serialize EncryptionInformation with EncryptionInformation.schema and a map of delete tokens
     * for each field in EncryptedFieldConfig.
     */
    static BSONObj encryptionInformationSerializeForDelete(const NamespaceString& nss,
                                                           const EncryptedFieldConfig& ef,
                                                           FLEKeyVault* keyVault);

    /**
     * Get a schema from EncryptionInformation and ensure the esc/ecc/ecoc are setup correctly.
     */
    static EncryptedFieldConfig getAndValidateSchema(const NamespaceString& nss,
                                                     const EncryptionInformation& ei);

    /**
     * Get a set of delete tokens for a given nss from EncryptionInformation.
     */
    static StringMap<FLEDeleteToken> getDeleteTokens(const NamespaceString& nss,
                                                     const EncryptionInformation& ei);
};

/**
 * A parsed element in the compaction tokens BSON object from
 * a compactStructuredEncryptionData command
 */
struct CompactionToken {
    std::string fieldPathName;
    ECOCToken token;
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
     * Merges the list of ECCDocuments so that entries whose tuple values are
     * adjacent to each other are combined into a single entry. For example,
     * the input [ (1,3), (11,11), (7,9), (4,6) ] outputs [ (1,9), (11,11) ].
     * Assumes none of the input entries overlap with each other.
     *
     * This will sort the input unmerged list as a side-effect.
     */
    static std::vector<ECCDocument> mergeECCDocuments(std::vector<ECCDocument>& unmerged);

    /**
     * Given a list of ECCDocument, where each document is a range of
     * deleted positions, this calculates the total number of deleted
     * positions.
     */
    static uint64_t countDeleted(const std::vector<ECCDocument>& rangeList);
};

/**
 * Split a ConstDataRange into a byte for EncryptedBinDataType and a ConstDataRange for the trailing
 * bytes
 *
 * Verifies that EncryptedBinDataType is valid.
 */
std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedConstDataRange(ConstDataRange cdr);

struct ParsedFindPayload {
    ESCDerivedFromDataToken escToken;
    ECCDerivedFromDataToken eccToken;
    EDCDerivedFromDataToken edcToken;
    boost::optional<ServerDataEncryptionLevel1Token> serverToken;
    boost::optional<std::int64_t> maxCounter;

    explicit ParsedFindPayload(BSONElement fleFindPayload);
    explicit ParsedFindPayload(const Value& fleFindPayload);
    explicit ParsedFindPayload(ConstDataRange cdr);
};


/**
 * FLE2 Range Utility functions
 */

/**
 * Describe the encoding of an BSON int32
 *
 * NOTE: It is not a mistake that a int32 is encoded as uint32.
 */
struct OSTType_Int32 {
    OSTType_Int32(uint32_t v, uint32_t minP, uint32_t maxP) : value(v), min(minP), max(maxP) {}

    uint32_t value;
    uint32_t min;
    uint32_t max;
};

OSTType_Int32 getTypeInfo32(int32_t value,
                            boost::optional<int32_t> min,
                            boost::optional<int32_t> max);

/**
 * Describe the encoding of an BSON int64
 *
 * NOTE: It is not a mistake that a int64 is encoded as uint64.
 */
struct OSTType_Int64 {
    OSTType_Int64(uint64_t v, uint64_t minP, uint64_t maxP) : value(v), min(minP), max(maxP) {}

    uint64_t value;
    uint64_t min;
    uint64_t max;
};

OSTType_Int64 getTypeInfo64(int64_t value,
                            boost::optional<int64_t> min,
                            boost::optional<int64_t> max);


/**
 * Describe the encoding of an BSON double (i.e. IEEE 754 Binary64)
 *
 * NOTE: It is not a mistake that a double is encoded as uint64.
 */
struct OSTType_Double {
    OSTType_Double(uint64_t v, uint64_t minP, uint64_t maxP) : value(v), min(minP), max(maxP) {}

    uint64_t value;
    uint64_t min;
    uint64_t max;
};

OSTType_Double getTypeInfoDouble(double value,
                                 boost::optional<double> min,
                                 boost::optional<double> max);


struct FLEFindEdgeTokenSet {
    EDCDerivedFromDataToken edc;
    ESCDerivedFromDataToken esc;
    ECCDerivedFromDataToken ecc;
};

struct ParsedFindRangePayload {
    std::vector<FLEFindEdgeTokenSet> edges;
    ServerDataEncryptionLevel1Token serverToken;
    std::int64_t maxCounter;

    explicit ParsedFindRangePayload(BSONElement fleFindRangePayload);
    explicit ParsedFindRangePayload(const Value& fleFindRangePayload);
    explicit ParsedFindRangePayload(ConstDataRange cdr);
};


/**
 * Edges calculator
 */

class Edges {
public:
    Edges(std::string leaf, int sparsity);
    std::vector<StringData> get();

private:
    std::string _leaf;
    int _sparsity;
};

std::unique_ptr<Edges> getEdgesInt32(int32_t value,
                                     boost::optional<int32_t> min,
                                     boost::optional<int32_t> max,
                                     int sparsity);

std::unique_ptr<Edges> getEdgesInt64(int64_t value,
                                     boost::optional<int64_t> min,
                                     boost::optional<int64_t> max,
                                     int sparsity);

std::unique_ptr<Edges> getEdgesDouble(double value,
                                      boost::optional<double> min,
                                      boost::optional<double> max,
                                      int sparsity);

/**
 * Mincover calculator
 */

std::vector<std::string> minCoverInt32(int32_t rangeMin,
                                       int32_t rangeMax,
                                       boost::optional<int32_t> min,
                                       boost::optional<int32_t> max,
                                       int sparsity);

std::vector<std::string> minCoverInt64(int64_t rangeMin,
                                       int64_t rangeMax,
                                       boost::optional<int64_t> min,
                                       boost::optional<int64_t> max,
                                       int sparsity);

std::vector<std::string> minCoverDouble(double rangeMin,
                                        double rangeMax,
                                        boost::optional<double> min,
                                        boost::optional<double> max,
                                        int sparsity);

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

bool hasQueryType(const EncryptedField& field, QueryTypeEnum queryType);
bool hasQueryType(const EncryptedFieldConfig& config, QueryTypeEnum queryType);

class EncryptedPredicateEvaluator {
public:
    EncryptedPredicateEvaluator(ConstDataRange serverToken,
                                int64_t contentionFactor,
                                std::vector<ConstDataRange> edcTokens);

    /**
     * Evaluates an encrypted predicate (equality or range), as defined by a contention factor along
     * with a list of edc tokens, over an encrypted value.
     *
     * Returns a boolean indicator.
     */
    template <typename T>
    bool evaluate(
        Value fieldValue,
        EncryptedBinDataType indexedValueType,
        std::function<StatusWith<T>(ConstDataRange, ConstDataRange)> decryptAndParse) const {

        if (fieldValue.getType() != BinData) {
            return false;
        }

        auto fieldValuePair = fromEncryptedBinData(fieldValue);

        uassert(
            6672400, "Invalid encrypted indexed field", fieldValuePair.first == indexedValueType);

        // Value matches if
        // 1. Decrypt field is successful
        // 2. EDC_u Token is in GenTokens(EDC Token, ContentionFactor)
        //
        auto swIndexed = decryptAndParse(ConstDataRange(_serverToken), fieldValuePair.second);
        uassertStatusOK(swIndexed);
        auto indexed = swIndexed.getValue();

        return _cachedEDCTokens.count(indexed.edc.data) == 1;
    }

    std::vector<PrfBlock> edcTokens() const {
        return _edcTokens;
    }

    PrfBlock serverToken() const {
        return _serverToken;
    }

    int64_t contentionFactor() const {
        return _contentionFactor;
    }

private:
    PrfBlock _serverToken;
    std::vector<PrfBlock> _edcTokens;
    int64_t _contentionFactor;
    stdx::unordered_set<PrfBlock> _cachedEDCTokens;
};

}  // namespace mongo
