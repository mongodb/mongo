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

#include "mongo/crypto/fle_crypto.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stack>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/idl/basic_types.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/object_check.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

// Optional defines to help with debugging
//
// Appends unencrypted fields to the state collections to aid in debugging
//#define FLE2_DEBUG_STATE_COLLECTIONS

// Verbose std::cout to troubleshoot the EmuBinary algorithm
//#define DEBUG_ENUM_BINARY 1

#ifdef FLE2_DEBUG_STATE_COLLECTIONS
static_assert(kDebugBuild == 1, "Only use in debug builds");
#endif

namespace mongo {

namespace {

constexpr uint64_t kLevel1Collection = 1;
constexpr uint64_t kLevel1ClientUserDataEncryption = 2;
constexpr uint64_t kLevelServerDataEncryption = 3;


constexpr uint64_t kEDC = 1;
constexpr uint64_t kESC = 2;
constexpr uint64_t kECC = 3;
constexpr uint64_t kECOC = 4;


constexpr uint64_t kTwiceDerivedTokenFromEDC = 1;
constexpr uint64_t kTwiceDerivedTokenFromESCTag = 1;
constexpr uint64_t kTwiceDerivedTokenFromESCValue = 2;
constexpr uint64_t kTwiceDerivedTokenFromECCTag = 1;
constexpr uint64_t kTwiceDerivedTokenFromECCValue = 2;

constexpr int32_t kEncryptionInformationSchemaVersion = 1;

constexpr auto kECCNullId = 0;
constexpr auto kECCNonNullId = 1;
constexpr uint64_t kECCompactionRecordValue = std::numeric_limits<uint64_t>::max();

constexpr uint64_t kESCNullId = 0;
constexpr uint64_t kESCNonNullId = 1;

constexpr uint64_t KESCInsertRecordValue = 0;
constexpr uint64_t kESCompactionRecordValue = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kESCompactionRecordCountPlaceholder = 0;

constexpr auto kId = "_id";
constexpr auto kValue = "value";
constexpr auto kFieldName = "fieldName";

constexpr auto kEncryptedFields = "encryptedFields";

#ifdef FLE2_DEBUG_STATE_COLLECTIONS
constexpr auto kDebugId = "_debug_id";
constexpr auto kDebugValuePosition = "_debug_value_position";
constexpr auto kDebugValueCount = "_debug_value_count";

constexpr auto kDebugValueStart = "_debug_value_start";
constexpr auto kDebugValueEnd = "_debug_value_end";
#endif

using UUIDBuf = std::array<uint8_t, UUID::kNumBytes>;

static_assert(sizeof(PrfBlock) == SHA256Block::kHashLength);
static_assert(sizeof(KeyMaterial) == crypto::sym256KeySize);

PrfBlock blockToArray(SHA256Block& block) {
    PrfBlock data;
    memcpy(data.data(), block.data(), sizeof(PrfBlock));
    return data;
}

PrfBlock PrfBlockfromCDR(ConstDataRange block) {
    uassert(6373501, "Invalid prf length", block.length() == sizeof(PrfBlock));

    PrfBlock ret;
    std::copy(block.data(), block.data() + block.length(), ret.data());
    return ret;
}

PrfBlock prf(ConstDataRange key, ConstDataRange cdr) {
    SHA256Block block;
    SHA256Block::computeHmac(key.data<uint8_t>(), key.length(), {cdr}, &block);
    return blockToArray(block);
}

PrfBlock prf(ConstDataRange key, uint64_t value) {
    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);

    return prf(key, bufValue);
}

PrfBlock prf(ConstDataRange key, uint64_t value, int64_t value2) {
    SHA256Block block;

    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);


    std::array<char, sizeof(uint64_t)> bufValue2;
    DataView(bufValue2.data()).write<LittleEndian<uint64_t>>(value2);

    SHA256Block::computeHmac(key.data<uint8_t>(),
                             key.length(),
                             {
                                 ConstDataRange{bufValue},
                                 ConstDataRange{bufValue2},
                             },
                             &block);
    return blockToArray(block);
}

ConstDataRange binDataToCDR(const BSONElement element) {
    uassert(6338501, "Expected binData BSON element", element.type() == BinData);

    int len;
    const char* data = element.binData(len);
    return ConstDataRange(data, data + len);
}

template <typename T>
void toBinData(StringData field, T t, BSONObjBuilder* builder) {
    BSONObj obj = t.toBSON();

    builder->appendBinData(field, obj.objsize(), BinDataType::BinDataGeneral, obj.objdata());
}

void toBinData(StringData field, PrfBlock block, BSONObjBuilder* builder) {
    builder->appendBinData(field, block.size(), BinDataType::BinDataGeneral, block.data());
}

void toBinData(StringData field, ConstDataRange block, BSONObjBuilder* builder) {
    builder->appendBinData(field, block.length(), BinDataType::BinDataGeneral, block.data());
}

void toBinData(StringData field, std::vector<uint8_t>& block, BSONObjBuilder* builder) {
    builder->appendBinData(field, block.size(), BinDataType::BinDataGeneral, block.data());
}

void appendTag(PrfBlock block, BSONArrayBuilder* builder) {
    builder->appendBinData(block.size(), BinDataType::BinDataGeneral, block.data());
}

template <typename T>
T parseFromCDR(ConstDataRange cdr) {
    ConstDataRangeCursor cdc(cdr);
    auto obj = cdc.readAndAdvance<Validated<BSONObj>>();

    IDLParserErrorContext ctx("root");
    return T::parse(ctx, obj);
}

std::vector<uint8_t> vectorFromCDR(ConstDataRange cdr) {
    std::vector<uint8_t> buf(cdr.length());
    std::copy(cdr.data(), cdr.data() + cdr.length(), buf.data());
    return buf;
}

template <typename T>
std::vector<uint8_t> toEncryptedVector(EncryptedBinDataType dt, T t) {
    BSONObj obj = t.toBSON();

    std::vector<uint8_t> buf(obj.objsize() + 1);
    buf[0] = static_cast<uint8_t>(dt);

    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), buf.data() + 1);

    return buf;
}

template <typename T>
void toEncryptedBinData(StringData field, EncryptedBinDataType dt, T t, BSONObjBuilder* builder) {
    auto buf = toEncryptedVector(dt, t);

    builder->appendBinData(field, buf.size(), BinDataType::Encrypt, buf.data());
}

void toEncryptedBinData(StringData field,
                        EncryptedBinDataType dt,
                        ConstDataRange cdr,
                        BSONObjBuilder* builder) {
    std::vector<uint8_t> buf(cdr.length() + 1);
    buf[0] = static_cast<uint8_t>(dt);

    std::copy(cdr.data(), cdr.data() + cdr.length(), buf.data() + 1);

    builder->appendBinData(field, buf.size(), BinDataType::Encrypt, buf.data());
}

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedConstDataRange(ConstDataRange cdr) {
    ConstDataRangeCursor cdrc(cdr);

    uint8_t subTypeByte = cdrc.readAndAdvance<uint8_t>();

    auto subType = EncryptedBinDataType_parse(IDLParserErrorContext("subtype"), subTypeByte);
    return {subType, cdrc};
}

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedBinData(BSONElement element) {
    uassert(
        6373502, "Expected binData with subtype Encrypt", element.isBinData(BinDataType::Encrypt));

    return fromEncryptedConstDataRange(binDataToCDR(element));
}

template <FLETokenType TokenT>
FLEToken<TokenT> FLETokenFromCDR(ConstDataRange cdr) {
    auto block = PrfBlockfromCDR(cdr);
    return FLEToken<TokenT>(block);
}

/**
 * AEAD AES + SHA256
 * Block size = 16 bytes
 * SHA-256 - block size = 256 bits = 32 bytes
 */
// TODO (SERVER-63780) - replace with call to CTR AEAD algorithm
StatusWith<std::vector<uint8_t>> encryptDataWithAssociatedData(ConstDataRange key,
                                                               ConstDataRange associatedData,
                                                               ConstDataRange plainText) {

    std::array<uint8_t, sizeof(uint64_t)> dataLenBitsEncodedStorage;
    DataRange dataLenBitsEncoded(dataLenBitsEncodedStorage);
    dataLenBitsEncoded.write<BigEndian<uint64_t>>(
        static_cast<uint64_t>(associatedData.length() * 8));

    std::vector<uint8_t> out;
    out.resize(crypto::aeadCipherOutputLength(plainText.length()));

    // TODO - key is too short, we have 32, need 64. The new API should only 32 bytes and this can
    // be removed
    std::array<uint8_t, 64> bigToken;
    std::copy(key.data(), key.data() + key.length(), bigToken.data());
    std::copy(key.data(), key.data() + key.length(), bigToken.data() + key.length());

    auto s = crypto::aeadEncryptWithIV(
        bigToken, plainText, ConstDataRange(0, 0), associatedData, dataLenBitsEncoded, out);
    if (!s.isOK()) {
        return s;
    }

    return {out};
}

// TODO (SERVER-63780) - replace with call to CTR algorithm, NOT CTR AEAD
StatusWith<std::vector<uint8_t>> encryptData(ConstDataRange key, ConstDataRange plainText) {

    return encryptDataWithAssociatedData(key, ConstDataRange(0, 0), plainText);
}

// TODO (SERVER-63780) - replace with call to CTR algorithm, NOT CTR AEAD
StatusWith<std::vector<uint8_t>> encryptData(ConstDataRange key, uint64_t value) {

    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);

    return encryptData(key, bufValue);
}

// TODO (SERVER-63780) - replace with call to CTR AEAD algorithm
StatusWith<std::vector<uint8_t>> decryptDataWithAssociatedData(ConstDataRange key,
                                                               ConstDataRange associatedData,
                                                               ConstDataRange cipherText) {
    // TODO - key is too short, we have 32, need 64. The new API should only 32 bytes and this can
    // be removed
    std::array<uint8_t, 64> bigToken;
    std::copy(key.data(), key.data() + key.length(), bigToken.data());
    std::copy(key.data(), key.data() + key.length(), bigToken.data() + key.length());

    SymmetricKey sk(reinterpret_cast<const uint8_t*>(bigToken.data()),
                    bigToken.size(),
                    0,
                    SymmetricKeyId("ignore"),
                    0);

    auto swLen = aeadGetMaximumPlainTextLength(cipherText.length());
    if (!swLen.isOK()) {
        return swLen.getStatus();
    }
    std::vector<uint8_t> out;
    out.resize(swLen.getValue());

    auto swOutLen = crypto::aeadDecrypt(sk, cipherText, associatedData, out);
    if (!swOutLen.isOK()) {
        return swOutLen.getStatus();
    }
    out.resize(swOutLen.getValue());
    return out;
}

// TODO (SERVER-63780) - replace with call to CTR algorithm, NOT CTR AEAD
StatusWith<std::vector<uint8_t>> decryptData(ConstDataRange key, ConstDataRange cipherText) {
    return decryptDataWithAssociatedData(key, ConstDataRange(0, 0), cipherText);
}

// TODO (SERVER-63780) - replace with call to CTR algorithm, NOT CTR AEAD
StatusWith<uint64_t> decryptUInt64(ConstDataRange key, ConstDataRange cipherText) {
    auto swPlainText = decryptData(key, cipherText);
    if (!swPlainText.isOK()) {
        return swPlainText.getStatus();
    }

    ConstDataRange cdr(swPlainText.getValue());

    return cdr.readNoThrow<LittleEndian<uint64_t>>();
}


template <typename T>
struct FLEStoragePackTypeHelper;

template <>
struct FLEStoragePackTypeHelper<uint64_t> {
    using Type = LittleEndian<uint64_t>;
};

template <>
struct FLEStoragePackTypeHelper<PrfBlock> {
    using Type = PrfBlock;
};

template <typename T>
struct FLEStoragePackType {
    // Note: the reference must be removed before the const
    using Type =
        typename FLEStoragePackTypeHelper<std::remove_const_t<std::remove_reference_t<T>>>::Type;
};

template <typename T1, typename T2, FLETokenType TokenT>
StatusWith<std::vector<uint8_t>> packAndEncrypt(std::tuple<T1, T2> tuple, FLEToken<TokenT> token) {
    DataBuilder builder(sizeof(T1) + sizeof(T2));
    Status s = builder.writeAndAdvance<typename FLEStoragePackType<T1>::Type>(std::get<0>(tuple));
    if (!s.isOK()) {
        return s;
    }

    s = builder.writeAndAdvance<typename FLEStoragePackType<T2>::Type>(std::get<1>(tuple));
    if (!s.isOK()) {
        return s;
    }

    dassert(builder.getCursor().length() == (sizeof(T1) + sizeof(T2)));
    return encryptData(token.toCDR(), builder.getCursor());
}


template <typename T1, typename T2, FLETokenType TokenT>
StatusWith<std::tuple<T1, T2>> decryptAndUnpack(ConstDataRange cdr, FLEToken<TokenT> token) {
    auto swVec = decryptData(token.toCDR(), cdr);
    if (!swVec.isOK()) {
        return swVec.getStatus();
    }

    auto& data = swVec.getValue();

    ConstDataRangeCursor cdrc(data);

    auto swt1 = cdrc.readAndAdvanceNoThrow<typename FLEStoragePackType<T1>::Type>();
    if (!swt1.isOK()) {
        return swt1.getStatus();
    }

    auto swt2 = cdrc.readAndAdvanceNoThrow<typename FLEStoragePackType<T2>::Type>();
    if (!swt2.isOK()) {
        return swt2.getStatus();
    }

    return std::tie(swt1.getValue(), swt2.getValue());
}


template <typename collectionT, typename tagTokenT, typename valueTokenT>
boost::optional<uint64_t> emuBinaryCommon(FLEStateCollectionReader* reader,
                                          tagTokenT tagToken,
                                          valueTokenT valueToken) {

    // Default search parameters
    uint64_t lambda = 0;
    boost::optional<uint64_t> i = 0;

    // Step 2:
    // Search for null record
    PrfBlock nullRecordId = collectionT::generateId(tagToken, boost::none);

    BSONObj nullDoc = reader->getById(nullRecordId);

    if (!nullDoc.isEmpty()) {
        auto swNullEscDoc = collectionT::decryptNullDocument(valueToken, nullDoc);
        uassertStatusOK(swNullEscDoc.getStatus());
        lambda = swNullEscDoc.getValue().position + 1;
        i = boost::none;
#ifdef DEBUG_ENUM_BINARY
        std::cout << fmt::format("start: null_document: lambda {}, i: {}", lambda, i) << std::endl;
#endif
    }

    // step 4, 5: get document count
    uint64_t rho = reader->getDocumentCount();

#ifdef DEBUG_ENUM_BINARY
    std::cout << fmt::format("start: lambda: {}, i: {}, rho: {}", lambda, i, rho) << std::endl;
#endif

    // step 6
    bool flag = true;

    // step 7
    // TODO - this loop never terminates unless it finds a document, need to add a terminating
    // condition
    while (flag) {
        // 7 a
        BSONObj doc = reader->getById(collectionT::generateId(tagToken, rho + lambda));

#ifdef DEBUG_ENUM_BINARY
        std::cout << fmt::format("search1: rho: {},  doc: {}", rho, doc.toString()) << std::endl;
#endif

        // 7 b
        if (!doc.isEmpty()) {
            rho = 2 * rho;
        } else {
            flag = false;
        }
    }

    // Step 8:
    uint64_t median = 0, min = 1, max = rho;

    // Step 9
    uint64_t maxIterations = rho > 0 ? ceil(log2(rho)) : 0;

#ifdef DEBUG_ENUM_BINARY
    std::cout << fmt::format("start2: maxIterations {}", maxIterations) << std::endl;
#endif

    for (uint64_t j = 1; j <= maxIterations; j++) {
        // 9a
        median = ceil(static_cast<double>(max - min) / 2) + min;


        // 9b
        BSONObj doc = reader->getById(collectionT::generateId(tagToken, median + lambda));

#ifdef DEBUG_ENUM_BINARY
        std::cout << fmt::format("search_stat: min: {}, median: {}, max: {}, i: {}, doc: {}",
                                 min,
                                 median,
                                 max,
                                 i,
                                 doc.toString())
                  << std::endl;
#endif

        // 9c
        if (!doc.isEmpty()) {
            // 9 c i
            min = median;

            // 9 c ii
            if (j == maxIterations) {
                i = min + lambda;
            }
            // 9d
        } else {
            // 9 d i
            max = median;

            // 9 d ii
            // Binary search has ended without finding a document, check for the first document
            // explicitly
            if (j == maxIterations && min == 1) {
                // 9 d ii A
                BSONObj doc = reader->getById(collectionT::generateId(tagToken, 1 + lambda));
                // 9 d ii B
                if (!doc.isEmpty()) {
                    i = 1 + lambda;
                }
            } else if (j == maxIterations && min != 1) {
                i = min + lambda;
            }
        }
    }

    return i;
}


/**
 * Stores a KeyId and encrypted value
 *
 * struct {
 *   uint8_t key_uuid[16];
 *   ciphertext[ciphertext_length];
 *  }
 */
class KeyIdAndValue {
public:
    static StatusWith<std::vector<uint8_t>> serialize(FLEUserKeyAndId userKey,
                                                      ConstDataRange value);
    /**
     * Read the key id from the payload.
     */
    static StatusWith<UUID> readKeyId(ConstDataRange cipherText);

    static StatusWith<std::vector<uint8_t>> decrypt(FLEUserKey userKey, ConstDataRange cipherText);
};

StatusWith<std::vector<uint8_t>> KeyIdAndValue::serialize(FLEUserKeyAndId userKey,
                                                          ConstDataRange value) {
    auto cdrKeyId = userKey.keyId.toCDR();

    auto swEncryptedData = encryptDataWithAssociatedData(userKey.key.toCDR(), cdrKeyId, value);
    if (!swEncryptedData.isOK()) {
        return swEncryptedData;
    }

    auto cipherText = swEncryptedData.getValue();
    std::vector<uint8_t> buf(cipherText.size() + cdrKeyId.length());

    std::copy(cdrKeyId.data<uint8_t>(), cdrKeyId.data<uint8_t>() + cdrKeyId.length(), buf.begin());
    std::copy(cipherText.begin(), cipherText.end(), buf.begin() + cdrKeyId.length());

    return buf;
}


StatusWith<UUID> KeyIdAndValue::readKeyId(ConstDataRange cipherText) {
    ConstDataRangeCursor baseCdrc(cipherText);


    auto swKeyId = baseCdrc.readAndAdvanceNoThrow<UUIDBuf>();
    if (!swKeyId.isOK()) {
        return {swKeyId.getStatus()};
    }

    return UUID::fromCDR(swKeyId.getValue());
}

StatusWith<std::vector<uint8_t>> KeyIdAndValue::decrypt(FLEUserKey userKey,
                                                        ConstDataRange cipherText) {

    ConstDataRangeCursor baseCdrc(cipherText);

    auto swKeyId = baseCdrc.readAndAdvanceNoThrow<UUIDBuf>();
    if (!swKeyId.isOK()) {
        return {swKeyId.getStatus()};
    }

    UUID keyId = UUID::fromCDR(swKeyId.getValue());

    return decryptDataWithAssociatedData(userKey.toCDR(), keyId.toCDR(), baseCdrc);
}

/**
 * Read and write FLE2InsertUpdate payload.
 */
class EDCClientPayload {
public:
    static FLE2InsertUpdatePayload parse(ConstDataRange cdr);
    static FLE2InsertUpdatePayload serialize(FLEIndexKeyAndId indexKey,
                                             FLEUserKeyAndId userKey,
                                             BSONElement element,
                                             uint64_t maxContentionCounter);
};

FLE2InsertUpdatePayload EDCClientPayload::parse(ConstDataRange cdr) {
    return parseFromCDR<FLE2InsertUpdatePayload>(cdr);
}

FLE2InsertUpdatePayload EDCClientPayload::serialize(FLEIndexKeyAndId indexKey,
                                                    FLEUserKeyAndId userKey,
                                                    BSONElement element,
                                                    uint64_t maxContentionCounter) {
    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey.key);
    auto serverEncryptToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(indexKey.key);
    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(collectionToken);
    auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collectionToken);

    EDCDerivedFromDataToken edcDatakey =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);
    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);
    ECCDerivedFromDataToken eccDatakey =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, value);

    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDatakey,
                                                                    maxContentionCounter);
    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey,
                                                                    maxContentionCounter);
    ECCDerivedFromDataTokenAndContentionFactorToken eccDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateECCDerivedFromDataTokenAndContentionFactorToken(eccDatakey,
                                                                    maxContentionCounter);


    FLE2InsertUpdatePayload iupayload;

    iupayload.setEdcDerivedToken(edcDataCounterkey.toCDR());
    iupayload.setEscDerivedToken(escDataCounterkey.toCDR());
    iupayload.setEccDerivedToken(eccDataCounterkey.toCDR());
    iupayload.setServerEncryptionToken(serverEncryptToken.toCDR());

    auto swEncryptedTokens =
        EncryptedStateCollectionTokens(escDataCounterkey, eccDataCounterkey).serialize(ecocToken);
    uassertStatusOK(swEncryptedTokens);
    iupayload.setEncryptedTokens(swEncryptedTokens.getValue());


    auto swCipherText = KeyIdAndValue::serialize(userKey, value);
    uassertStatusOK(swCipherText);
    iupayload.setValue(swCipherText.getValue());
    iupayload.setType(element.type());

    iupayload.setIndexKeyId(indexKey.keyId);

    return iupayload;
}


/**
 * Lightweight class to build a singly linked list of field names to represent the current field
 * name
 *
 * Avoids heap allocations until getFieldPath() is called
 */
class SinglyLinkedFieldPath {
public:
    SinglyLinkedFieldPath() : _predecessor(nullptr) {}

    SinglyLinkedFieldPath(StringData fieldName, const SinglyLinkedFieldPath* predecessor)
        : _currentField(fieldName), _predecessor(predecessor) {}


    std::string getFieldPath(StringData fieldName) const;

private:
    // Name of the current field that is being parsed.
    const StringData _currentField;

    // Pointer to a parent parser context.
    // This provides a singly linked list of parent pointers, and use to produce a full path to a
    // field with an error.
    const SinglyLinkedFieldPath* _predecessor;
};


std::string SinglyLinkedFieldPath::getFieldPath(StringData fieldName) const {
    dassert(!fieldName.empty());
    if (_predecessor == nullptr) {
        str::stream builder;

        if (!_currentField.empty()) {
            builder << _currentField << ".";
        }

        builder << fieldName;

        return builder;
    } else {
        std::stack<StringData> pieces;

        pieces.push(fieldName);

        if (!_currentField.empty()) {
            pieces.push(_currentField);
        }

        const SinglyLinkedFieldPath* head = _predecessor;
        while (head) {
            if (!head->_currentField.empty()) {
                pieces.push(head->_currentField);
            }
            head = head->_predecessor;
        }

        str::stream builder;

        while (!pieces.empty()) {
            builder << pieces.top();
            pieces.pop();

            if (!pieces.empty()) {
                builder << ".";
            }
        }

        return builder;
    }
}

/**
 * Copies an input document to the output but provides callers a way to customize how encrypted
 * fields are handled.
 *
 * Callers can pass a function doTransform(Original bindata content, BSONObjBuilder, Field Name).
 *
 * Function is expected to append a field with the specified "Field Name" to the output.
 */
BSONObj transformBSON(
    const BSONObj& object,
    const std::function<void(ConstDataRange, BSONObjBuilder*, StringData)>& doTransform) {
    struct IteratorState {
        BSONObjIterator iter;
        BSONObjBuilder builder;
    };

    std::stack<IteratorState> frameStack;

    const ScopeGuard frameStackGuard([&] {
        while (!frameStack.empty()) {
            frameStack.pop();
        }
    });

    frameStack.push({BSONObjIterator(object), BSONObjBuilder()});

    while (frameStack.size() > 1 || frameStack.top().iter.more()) {
        uassert(6338601,
                "Object too deep to be encrypted. Exceeded stack depth.",
                frameStack.size() < BSONDepth::kDefaultMaxAllowableDepth);
        auto& [iterator, builder] = frameStack.top();
        if (iterator.more()) {
            BSONElement elem = iterator.next();
            if (elem.type() == BSONType::Object) {
                frameStack.push({BSONObjIterator(elem.Obj()),
                                 BSONObjBuilder(builder.subobjStart(elem.fieldNameStringData()))});
            } else if (elem.type() == BSONType::Array) {
                frameStack.push(
                    {BSONObjIterator(elem.Obj()),
                     BSONObjBuilder(builder.subarrayStart(elem.fieldNameStringData()))});
            } else if (elem.isBinData(BinDataType::Encrypt)) {
                int len;
                const char* data(elem.binData(len));
                ConstDataRange cdr(data, len);
                doTransform(cdr, &builder, elem.fieldNameStringData());
            } else {
                builder.append(elem);
            }
        } else {
            frameStack.pop();
        }
    }

    invariant(frameStack.size() == 1);

    return frameStack.top().builder.obj();
}

/**
 * Iterates through all encrypted fields. Does not return a document like doTransform.
 *
 * Callers can pass a function doVisit(Original bindata content, Field Name).
 */
void visitEncryptedBSON(const BSONObj& object,
                        const std::function<void(ConstDataRange, std::string)>& doVisit) {
    std::stack<std::pair<SinglyLinkedFieldPath, BSONObjIterator>> frameStack;

    const ScopeGuard frameStackGuard([&] {
        while (!frameStack.empty()) {
            frameStack.pop();
        }
    });

    frameStack.emplace(SinglyLinkedFieldPath(), BSONObjIterator(object));

    while (frameStack.size() > 1 || frameStack.top().second.more()) {
        uassert(6373511,
                "Object too deep to be encrypted. Exceeded stack depth.",
                frameStack.size() < BSONDepth::kDefaultMaxAllowableDepth);
        auto& iterator = frameStack.top();
        if (iterator.second.more()) {
            BSONElement elem = iterator.second.next();
            if (elem.type() == BSONType::Object) {
                frameStack.emplace(
                    SinglyLinkedFieldPath(elem.fieldNameStringData(), &iterator.first),
                    BSONObjIterator(elem.Obj()));
            } else if (elem.type() == BSONType::Array) {
                frameStack.emplace(
                    SinglyLinkedFieldPath(elem.fieldNameStringData(), &iterator.first),
                    BSONObjIterator(elem.Obj()));
            } else if (elem.isBinData(BinDataType::Encrypt)) {
                int len;
                const char* data(elem.binData(len));
                ConstDataRange cdr(data, len);
                doVisit(cdr, iterator.first.getFieldPath(elem.fieldNameStringData()));
            }
        } else {
            frameStack.pop();
        }
    }
    invariant(frameStack.size() == 1);
}

/**
 * Converts an encryption placeholder to FLE2InsertUpdatePayload in prepration for insert,
 * fndAndModify and update.
 */
void convertToFLE2InsertUpdateValue(FLEKeyVault* keyVault,
                                    ConstDataRange cdr,
                                    BSONObjBuilder* builder,
                                    StringData fieldNameToSerialize) {
    auto [encryptedType, subCdr] = fromEncryptedConstDataRange(cdr);

    if (encryptedType == EncryptedBinDataType::kFLE2Placeholder) {

        auto ep = parseFromCDR<FLE2EncryptionPlaceholder>(subCdr);

        auto el = ep.getValue().getElement();


        FLEIndexKeyAndId indexKey = keyVault->getIndexKeyById(ep.getIndexKeyId());
        FLEUserKeyAndId userKey = keyVault->getUserKeyById(ep.getUserKeyId());

        if (ep.getAlgorithm() == Fle2AlgorithmInt::kEquality) {
            uassert(6338602,
                    str::stream() << "Type '" << typeName(el.type())
                                  << "' is not a valid type for FLE 2 encryption",
                    isFLE2EqualityIndexedSupportedType(el.type()));

            auto iupayload =
                EDCClientPayload::serialize(indexKey, userKey, el, ep.getMaxContentionCounter());
            toEncryptedBinData(fieldNameToSerialize,
                               EncryptedBinDataType::kFLE2InsertUpdatePayload,
                               iupayload,
                               builder);

        } else {
            uasserted(6338603, "Only FLE 2 style encryption placeholders are supported");
        }


    } else {
        // TODO - validate acceptable types - kFLE2Placeholder or kFLE2UnindexedEncryptedValue or
        // kFLE2EqualityIndexedValue
        toEncryptedBinData(fieldNameToSerialize, encryptedType, subCdr, builder);
    }
}

void collectEDCServerInfo(std::vector<EDCServerPayloadInfo>* pFields,
                          ConstDataRange cdr,
                          StringData fieldPath) {

    // TODO - validate acceptable types - kFLE2InsertUpdatePayload or kFLE2UnindexedEncryptedValue
    // or kFLE2EqualityIndexedValue
    // TODO - validate field is actually indexed in the schema?

    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);

    auto encryptedType = encryptedTypeBinding;
    uassert(6373503,
            str::stream() << "Unexpected encrypted payload type: "
                          << static_cast<uint32_t>(encryptedType),
            encryptedType == EncryptedBinDataType::kFLE2InsertUpdatePayload);

    auto iupayload = EDCClientPayload::parse(subCdr);

    uassert(6373504,
            str::stream() << "Type '" << typeName(static_cast<BSONType>(iupayload.getType()))
                          << "' is not a valid type for FLE 2 encryption",
            isValidBSONType(iupayload.getType()) &&
                isFLE2EqualityIndexedSupportedType(static_cast<BSONType>(iupayload.getType())));

    pFields->push_back({std::move(iupayload), fieldPath.toString(), 0});
}

template <typename T>
struct ConstVectorIteratorPair {
    ConstVectorIteratorPair(const std::vector<T>& vec) : it(vec.cbegin()), end(vec.cend()) {}

    typename std::vector<T>::const_iterator it;
    typename std::vector<T>::const_iterator end;
};

struct TagInfo {
    PrfBlock tag;
};

void convertServerPayload(std::vector<TagInfo>* pTags,
                          ConstVectorIteratorPair<EDCServerPayloadInfo>& it,
                          BSONObjBuilder* builder,
                          StringData fieldPath) {

    uassert(6373505, "Unexpected end of iterator", it.it != it.end);
    auto payload = *(it.it);

    // TODO - validate acceptable types - kFLE2InsertUpdatePayload or kFLE2UnindexedEncryptedValue
    // or kFLE2EqualityIndexedValue
    // TODO - validate field is actually indexed in the schema?

    FLE2IndexedEqualityEncryptedValue sp(payload.payload, payload.count);

    uassert(6373506,
            str::stream() << "Type '" << typeName(sp.bsonType)
                          << "' is not a valid type for FLE 2 encryption",
            isFLE2EqualityIndexedSupportedType(sp.bsonType));

    auto swEncrypted = sp.serialize(FLETokenFromCDR<FLETokenType::ServerDataEncryptionLevel1Token>(
        payload.payload.getServerEncryptionToken()));
    uassertStatusOK(swEncrypted);
    toEncryptedBinData(fieldPath,
                       EncryptedBinDataType::kFLE2EqualityIndexedValue,
                       ConstDataRange(swEncrypted.getValue()),
                       builder);

    pTags->push_back({EDCServerCollection::generateTag(payload)});

    it.it++;
}


BSONObj toBSON(BSONType type, ConstDataRange cdr) {
    auto valueString = "value"_sd;

    // The size here is to construct a new BSON document and validate the
    // total size of the object. The first four bytes is for the size of an
    // int32_t, then a space for the type of the first element, then the space
    // for the value string and the the 0x00 terminated field name, then the
    // size of the actual data, then the last byte for the end document character,
    // also 0x00.
    size_t docLength = sizeof(int32_t) + 1 + valueString.size() + 1 + cdr.length() + 1;
    BufBuilder builder;
    builder.reserveBytes(docLength);

    uassert(ErrorCodes::BadValue,
            "invalid decryption value",
            docLength < std::numeric_limits<int32_t>::max());

    builder.appendNum(static_cast<uint32_t>(docLength));
    builder.appendChar(static_cast<uint8_t>(type));
    builder.appendStr(valueString, true);
    builder.appendBuf(cdr.data(), cdr.length());
    builder.appendChar('\0');

    ConstDataRangeCursor cdc = ConstDataRangeCursor(ConstDataRange(builder.buf(), builder.len()));
    BSONObj elemWrapped = cdc.readAndAdvance<Validated<BSONObj>>();
    return elemWrapped.getOwned();
}


void decryptField(FLEKeyVault* keyVault,
                  ConstDataRange cdr,
                  BSONObjBuilder* builder,
                  StringData fieldPath) {

    auto pair = FLEClientCrypto::decrypt(cdr, keyVault);

    BSONObj obj = toBSON(pair.first, pair.second);

    builder->appendAs(obj.firstElement(), fieldPath);
}

void collectIndexedFields(std::vector<EDCIndexedFields>* pFields,
                          ConstDataRange cdr,
                          StringData fieldPath) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
    if (encryptedTypeBinding != EncryptedBinDataType::kFLE2EqualityIndexedValue) {
        return;
    }

    pFields->push_back({cdr, fieldPath.toString()});
}

}  // namespace


CollectionsLevel1Token FLELevel1TokenGenerator::generateCollectionsLevel1Token(
    FLEIndexKey indexKey) {
    return prf(indexKey.data, kLevel1Collection);
}

ServerDataEncryptionLevel1Token FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(
    FLEIndexKey indexKey) {
    return prf(indexKey.data, kLevelServerDataEncryption);
}


EDCToken FLECollectionTokenGenerator::generateEDCToken(CollectionsLevel1Token token) {
    return prf(token.data, kEDC);
}

ESCToken FLECollectionTokenGenerator::generateESCToken(CollectionsLevel1Token token) {
    return prf(token.data, kESC);
}

ECCToken FLECollectionTokenGenerator::generateECCToken(CollectionsLevel1Token token) {
    return prf(token.data, kECC);
}

ECOCToken FLECollectionTokenGenerator::generateECOCToken(CollectionsLevel1Token token) {
    return prf(token.data, kECOC);
}


EDCDerivedFromDataToken FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(
    EDCToken token, ConstDataRange value) {
    return prf(token.data, value);
}

ESCDerivedFromDataToken FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(
    ESCToken token, ConstDataRange value) {
    return prf(token.data, value);
}

ECCDerivedFromDataToken FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(
    ECCToken token, ConstDataRange value) {
    return prf(token.data, value);
}


EDCDerivedFromDataTokenAndContentionFactorToken
FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
    generateEDCDerivedFromDataTokenAndContentionFactorToken(EDCDerivedFromDataToken token,
                                                            FLECounter counter) {
    return prf(token.data, counter);
}

ESCDerivedFromDataTokenAndContentionFactorToken
FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
    generateESCDerivedFromDataTokenAndContentionFactorToken(ESCDerivedFromDataToken token,
                                                            FLECounter counter) {
    return prf(token.data, counter);
}

ECCDerivedFromDataTokenAndContentionFactorToken
FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
    generateECCDerivedFromDataTokenAndContentionFactorToken(ECCDerivedFromDataToken token,
                                                            FLECounter counter) {
    return prf(token.data, counter);
}


EDCTwiceDerivedToken FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(
    EDCDerivedFromDataTokenAndContentionFactorToken token) {
    return prf(token.data, kTwiceDerivedTokenFromEDC);
}

ESCTwiceDerivedTagToken FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(
    ESCDerivedFromDataTokenAndContentionFactorToken token) {
    return prf(token.data, kTwiceDerivedTokenFromESCTag);
}

ESCTwiceDerivedValueToken FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(
    ESCDerivedFromDataTokenAndContentionFactorToken token) {
    return prf(token.data, kTwiceDerivedTokenFromESCValue);
}

ECCTwiceDerivedTagToken FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(
    ECCDerivedFromDataTokenAndContentionFactorToken token) {
    return prf(token.data, kTwiceDerivedTokenFromECCTag);
}

ECCTwiceDerivedValueToken FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(
    ECCDerivedFromDataTokenAndContentionFactorToken token) {
    return prf(token.data, kTwiceDerivedTokenFromECCValue);
}

StatusWith<EncryptedStateCollectionTokens> EncryptedStateCollectionTokens::decryptAndParse(
    ECOCToken token, ConstDataRange cdr) {
    auto swUnpack = decryptAndUnpack<PrfBlock, PrfBlock>(cdr, token);

    if (!swUnpack.isOK()) {
        return swUnpack.getStatus();
    }

    auto value = swUnpack.getValue();

    return EncryptedStateCollectionTokens{
        ESCDerivedFromDataTokenAndContentionFactorToken(std::get<0>(value)),
        ECCDerivedFromDataTokenAndContentionFactorToken(std::get<1>(value))};
}

StatusWith<std::vector<uint8_t>> EncryptedStateCollectionTokens::serialize(ECOCToken token) {
    return packAndEncrypt(std::tie(esc.data, ecc.data), token);
}

FLEKeyVault::~FLEKeyVault() {}


std::vector<uint8_t> FLEClientCrypto::encrypt(BSONElement element,
                                              FLEIndexKeyAndId indexKey,
                                              FLEUserKeyAndId userKey,
                                              FLECounter counter) {

    auto iupayload = EDCClientPayload::serialize(indexKey, userKey, element, counter);

    return toEncryptedVector(EncryptedBinDataType::kFLE2InsertUpdatePayload, iupayload);
}


BSONObj FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(const BSONObj& obj,
                                                                FLEKeyVault* keyVault) {
    auto ret = transformBSON(
        obj, [keyVault](ConstDataRange cdr, BSONObjBuilder* builder, StringData field) {
            convertToFLE2InsertUpdateValue(keyVault, cdr, builder, field);
        });

    return ret;
}


std::pair<BSONType, std::vector<uint8_t>> FLEClientCrypto::decrypt(BSONElement element,
                                                                   FLEKeyVault* keyVault) {
    auto pair = fromEncryptedBinData(element);

    return FLEClientCrypto::decrypt(pair.second, keyVault);
}

std::pair<BSONType, std::vector<uint8_t>> FLEClientCrypto::decrypt(ConstDataRange cdr,
                                                                   FLEKeyVault* keyVault) {
    auto pair = fromEncryptedConstDataRange(cdr);

    if (pair.first == EncryptedBinDataType::kFLE2EqualityIndexedValue) {
        auto indexKeyId =
            uassertStatusOK(FLE2IndexedEqualityEncryptedValue::readKeyId(pair.second));

        auto indexKey = keyVault->getIndexKeyById(indexKeyId);

        auto serverDataToken =
            FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(indexKey.key);

        auto ieev = uassertStatusOK(
            FLE2IndexedEqualityEncryptedValue::decryptAndParse(serverDataToken, pair.second));

        auto userCipherText = ieev.clientEncryptedValue;

        auto userKeyId = uassertStatusOK(KeyIdAndValue::readKeyId(userCipherText));

        auto userKey = keyVault->getUserKeyById(userKeyId);

        auto userData = uassertStatusOK(KeyIdAndValue::decrypt(userKey.key, userCipherText));

        return {ieev.bsonType, userData};

    } else {
        uasserted(6373507, "Not supported");
    }

    return {EOO, std::vector<uint8_t>()};
}

BSONObj FLEClientCrypto::decryptDocument(BSONObj& doc, FLEKeyVault* keyVault) {

    BSONObjBuilder builder;

    // TODO - validate acceptable types - kFLE2UnindexedEncryptedValue or kFLE2EqualityIndexedValue
    // kFLE2InsertUpdatePayload?
    auto obj = transformBSON(
        doc, [keyVault](ConstDataRange cdr, BSONObjBuilder* builder, StringData fieldPath) {
            decryptField(keyVault, cdr, builder, fieldPath);
        });

    builder.appendElements(obj);

    return builder.obj();
}

PrfBlock ESCCollection::generateId(ESCTwiceDerivedTagToken tagToken,
                                   boost::optional<uint64_t> index) {
    if (index.has_value()) {
        return prf(tagToken.data, kESCNonNullId, index.value());
    } else {
        return prf(tagToken.data, kESCNullId, 0);
    }
}

BSONObj ESCCollection::generateNullDocument(ESCTwiceDerivedTagToken tagToken,
                                            ESCTwiceDerivedValueToken valueToken,
                                            uint64_t pos,
                                            uint64_t count) {
    auto block = ESCCollection::generateId(tagToken, boost::none);

    auto swCipherText = packAndEncrypt(std::tie(pos, count), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
#ifdef FLE2_DEBUG_STATE_COLLECTIONS
    builder.append(kDebugId, "NULL DOC");
    builder.append(kDebugValuePosition, static_cast<int64_t>(pos));
    builder.append(kDebugValueCount, static_cast<int64_t>(count));
#endif

    return builder.obj();
}


BSONObj ESCCollection::generateInsertDocument(ESCTwiceDerivedTagToken tagToken,
                                              ESCTwiceDerivedValueToken valueToken,
                                              uint64_t index,
                                              uint64_t count) {
    auto block = ESCCollection::generateId(tagToken, index);

    auto swCipherText = packAndEncrypt(std::tie(KESCInsertRecordValue, count), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
#ifdef FLE2_DEBUG_STATE_COLLECTIONS
    builder.append(kDebugId, static_cast<int64_t>(index));
    builder.append(kDebugValueCount, static_cast<int64_t>(count));
#endif

    return builder.obj();
}


BSONObj ESCCollection::generatePositionalDocument(ESCTwiceDerivedTagToken tagToken,
                                                  ESCTwiceDerivedValueToken valueToken,
                                                  uint64_t index,
                                                  uint64_t pos,
                                                  uint64_t count) {
    auto block = ESCCollection::generateId(tagToken, index);

    auto swCipherText = packAndEncrypt(std::tie(pos, count), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
#ifdef FLE2_DEBUG_STATE_COLLECTIONS
    builder.append(kDebugId, static_cast<int64_t>(index));
    builder.append(kDebugValuePosition, static_cast<int64_t>(pos));
    builder.append(kDebugValueCount, static_cast<int64_t>(count));
#endif

    return builder.obj();
}


BSONObj ESCCollection::generateCompactionPlaceholderDocument(ESCTwiceDerivedTagToken tagToken,
                                                             ESCTwiceDerivedValueToken valueToken,
                                                             uint64_t index) {
    auto block = ESCCollection::generateId(tagToken, index);

    auto swCipherText = packAndEncrypt(
        std::tie(kESCompactionRecordValue, kESCompactionRecordCountPlaceholder), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);

    return builder.obj();
}

StatusWith<ESCNullDocument> ESCCollection::decryptNullDocument(ESCTwiceDerivedValueToken valueToken,
                                                               BSONObj& doc) {
    BSONElement encryptedValue;
    auto status = bsonExtractTypedField(doc, kValue, BinData, &encryptedValue);
    if (!status.isOK()) {
        return status;
    }

    auto swUnpack = decryptAndUnpack<uint64_t, uint64_t>(binDataToCDR(encryptedValue), valueToken);

    if (!swUnpack.isOK()) {
        return swUnpack.getStatus();
    }

    auto& value = swUnpack.getValue();

    return ESCNullDocument{std::get<0>(value), std::get<1>(value)};
}


StatusWith<ESCDocument> ESCCollection::decryptDocument(ESCTwiceDerivedValueToken valueToken,
                                                       BSONObj& doc) {
    BSONElement encryptedValue;
    auto status = bsonExtractTypedField(doc, kValue, BinData, &encryptedValue);
    if (!status.isOK()) {
        return status;
    }

    auto swUnpack = decryptAndUnpack<uint64_t, uint64_t>(binDataToCDR(encryptedValue), valueToken);

    if (!swUnpack.isOK()) {
        return swUnpack.getStatus();
    }

    auto& value = swUnpack.getValue();

    return ESCDocument{
        std::get<0>(value) == kESCompactionRecordValue, std::get<0>(value), std::get<1>(value)};
}


boost::optional<uint64_t> ESCCollection::emuBinary(FLEStateCollectionReader* reader,
                                                   ESCTwiceDerivedTagToken tagToken,
                                                   ESCTwiceDerivedValueToken valueToken) {
    return emuBinaryCommon<ESCCollection, ESCTwiceDerivedTagToken, ESCTwiceDerivedValueToken>(
        reader, tagToken, valueToken);
}


PrfBlock ECCCollection::generateId(ECCTwiceDerivedTagToken tagToken,
                                   boost::optional<uint64_t> index) {
    if (index.has_value()) {
        return prf(tagToken.data, kECCNonNullId, index.value());
    } else {
        return prf(tagToken.data, kECCNullId, 0);
    }
}

BSONObj ECCCollection::generateNullDocument(ECCTwiceDerivedTagToken tagToken,
                                            ECCTwiceDerivedValueToken valueToken,
                                            uint64_t count) {
    auto block = ECCCollection::generateId(tagToken, boost::none);

    auto swCipherText = encryptData(valueToken.data, count);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
#ifdef FLE2_DEBUG_STATE_COLLECTIONS
    builder.append(kDebugId, "NULL DOC");
    builder.append(kDebugValueCount, static_cast<int64_t>(count));
#endif

    return builder.obj();
}

BSONObj ECCCollection::generateDocument(ECCTwiceDerivedTagToken tagToken,
                                        ECCTwiceDerivedValueToken valueToken,
                                        uint64_t index,
                                        uint64_t start,
                                        uint64_t end) {
    auto block = ECCCollection::generateId(tagToken, index);

    auto swCipherText = packAndEncrypt(std::tie(start, end), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
#ifdef FLE2_DEBUG_STATE_COLLECTIONS
    builder.append(kDebugId, static_cast<int64_t>(index));
    builder.append(kDebugValueStart, static_cast<int64_t>(start));
    builder.append(kDebugValueEnd, static_cast<int64_t>(end));
#endif

    return builder.obj();
}

BSONObj ECCCollection::generateDocument(ECCTwiceDerivedTagToken tagToken,
                                        ECCTwiceDerivedValueToken valueToken,
                                        uint64_t index,
                                        uint64_t count) {
    return generateDocument(tagToken, valueToken, index, count, count);
}

BSONObj ECCCollection::generateCompactionDocument(ECCTwiceDerivedTagToken tagToken,
                                                  ECCTwiceDerivedValueToken valueToken,
                                                  uint64_t index) {
    auto block = ECCCollection::generateId(tagToken, index);

    auto swCipherText =
        packAndEncrypt(std::tie(kECCompactionRecordValue, kECCompactionRecordValue), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
#ifdef FLE2_DEBUG_STATE_COLLECTIONS
    builder.append(kDebugId, static_cast<int64_t>(index));
    builder.append(kDebugValueStart, static_cast<int64_t>(kECCompactionRecordValue));
    builder.append(kDebugValueEnd, static_cast<int64_t>(kECCompactionRecordValue));
#endif

    return builder.obj();
}


StatusWith<ECCNullDocument> ECCCollection::decryptNullDocument(ECCTwiceDerivedValueToken valueToken,
                                                               BSONObj& doc) {
    BSONElement encryptedValue;
    auto status = bsonExtractTypedField(doc, kValue, BinData, &encryptedValue);
    if (!status.isOK()) {
        return status;
    }

    auto swUnpack = decryptUInt64(valueToken.data, binDataToCDR(encryptedValue));

    if (!swUnpack.isOK()) {
        return swUnpack.getStatus();
    }

    auto& value = swUnpack.getValue();

    return ECCNullDocument{value};
}


StatusWith<ECCDocument> ECCCollection::decryptDocument(ECCTwiceDerivedValueToken valueToken,
                                                       BSONObj& doc) {
    BSONElement encryptedValue;
    auto status = bsonExtractTypedField(doc, kValue, BinData, &encryptedValue);
    if (!status.isOK()) {
        return status;
    }

    auto swUnpack = decryptAndUnpack<uint64_t, uint64_t>(binDataToCDR(encryptedValue), valueToken);

    if (!swUnpack.isOK()) {
        return swUnpack.getStatus();
    }

    auto& value = swUnpack.getValue();

    return ECCDocument{std::get<0>(value) != kECCompactionRecordValue
                           ? ECCValueType::kNormal
                           : ECCValueType::kCompactionPlaceholder,
                       std::get<0>(value),
                       std::get<1>(value)};
}

boost::optional<uint64_t> ECCCollection::emuBinary(FLEStateCollectionReader* reader,
                                                   ECCTwiceDerivedTagToken tagToken,
                                                   ECCTwiceDerivedValueToken valueToken) {
    return emuBinaryCommon<ECCCollection, ECCTwiceDerivedTagToken, ECCTwiceDerivedValueToken>(
        reader, tagToken, valueToken);
}

BSONObj ECOCollection::generateDocument(StringData fieldName, ConstDataRange payload) {
    BSONObjBuilder builder;
    builder.append(kId, OID::gen());
    builder.append(kFieldName, fieldName);
    toBinData(kValue, payload, &builder);
    return builder.obj();
}

ECOCCompactionDocument ECOCollection::parseAndDecrypt(BSONObj& doc, ECOCToken token) {
    IDLParserErrorContext ctx("root");
    auto ecocDoc = EcocDocument::parse(ctx, doc);

    auto swTokens = EncryptedStateCollectionTokens::decryptAndParse(token, ecocDoc.getValue());
    uassertStatusOK(swTokens);
    auto& keys = swTokens.getValue();

    ECOCCompactionDocument ret;
    ret.fieldName = ecocDoc.getFieldName().toString();
    ret.esc = keys.esc;
    ret.ecc = keys.ecc;
    return ret;
}

FLE2IndexedEqualityEncryptedValue::FLE2IndexedEqualityEncryptedValue(
    FLE2InsertUpdatePayload payload, uint64_t counter)
    : edc(FLETokenFromCDR<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>(
          payload.getEdcDerivedToken())),
      esc(FLETokenFromCDR<FLETokenType::ESCDerivedFromDataTokenAndContentionFactorToken>(
          payload.getEscDerivedToken())),
      ecc(FLETokenFromCDR<FLETokenType::ECCDerivedFromDataTokenAndContentionFactorToken>(
          payload.getEccDerivedToken())),
      count(counter),
      bsonType(static_cast<BSONType>(payload.getType())),
      indexKeyId(payload.getIndexKeyId()),
      clientEncryptedValue(vectorFromCDR(payload.getValue())) {
    uassert(6373508,
            "Invalid BSON Type in FLE2InsertUpdatePayload",
            isValidBSONType(payload.getType()));
}

FLE2IndexedEqualityEncryptedValue::FLE2IndexedEqualityEncryptedValue(
    EDCDerivedFromDataTokenAndContentionFactorToken edcParam,
    ESCDerivedFromDataTokenAndContentionFactorToken escParam,
    ECCDerivedFromDataTokenAndContentionFactorToken eccParam,
    uint64_t countParam,
    BSONType typeParam,
    UUID indexKeyIdParam,
    std::vector<uint8_t> clientEncryptedValueParam)
    : edc(edcParam),
      esc(escParam),
      ecc(eccParam),
      count(countParam),
      bsonType(typeParam),
      indexKeyId(indexKeyIdParam),
      clientEncryptedValue(clientEncryptedValueParam) {}

StatusWith<UUID> FLE2IndexedEqualityEncryptedValue::readKeyId(
    ConstDataRange serializedServerValue) {
    ConstDataRangeCursor baseCdrc(serializedServerValue);

    auto swKeyId = baseCdrc.readAndAdvanceNoThrow<UUIDBuf>();
    if (!swKeyId.isOK()) {
        return {swKeyId.getStatus()};
    }

    return UUID::fromCDR(swKeyId.getValue());
}

StatusWith<FLE2IndexedEqualityEncryptedValue> FLE2IndexedEqualityEncryptedValue::decryptAndParse(
    ServerDataEncryptionLevel1Token token, ConstDataRange serializedServerValue) {

    ConstDataRangeCursor serializedServerCdrc(serializedServerValue);

    auto swIndexKeyId = serializedServerCdrc.readAndAdvanceNoThrow<UUIDBuf>();
    if (!swIndexKeyId.isOK()) {
        return {swIndexKeyId.getStatus()};
    }

    UUID indexKey = UUID::fromCDR(swIndexKeyId.getValue());

    auto swBsonType = serializedServerCdrc.readAndAdvanceNoThrow<uint8_t>();
    if (!swBsonType.isOK()) {
        return {swBsonType.getStatus()};
    }

    uassert(6373509,
            "Invalid BSON Type in FLE2InsertUpdatePayload",
            isValidBSONType(swBsonType.getValue()));

    auto type = static_cast<BSONType>(swBsonType.getValue());

    auto swVec = decryptData(token.toCDR(), serializedServerCdrc);
    if (!swVec.isOK()) {
        return swVec.getStatus();
    }

    auto data = swVec.getValue();

    ConstDataRangeCursor serverEncryptedValueCdrc(data);

    auto swLength = serverEncryptedValueCdrc.readAndAdvanceNoThrow<LittleEndian<std::uint64_t>>();
    if (!swLength.isOK()) {
        return {swLength.getStatus()};
    }

    std::uint64_t length = swLength.getValue();

    auto start = serverEncryptedValueCdrc.data();

    auto advance = serverEncryptedValueCdrc.advanceNoThrow(length);
    if (!advance.isOK()) {
        return advance;
    }

    std::vector<uint8_t> cipherText(length);

    std::copy(start, start + length, cipherText.data());

    auto swCount = serverEncryptedValueCdrc.readAndAdvanceNoThrow<LittleEndian<std::uint64_t>>();
    if (!swCount.isOK()) {
        return {swCount.getStatus()};
    }

    auto swEdc = serverEncryptedValueCdrc.readAndAdvanceNoThrow<PrfBlock>();
    if (!swEdc.isOK()) {
        return swEdc.getStatus();
    }

    auto swEsc = serverEncryptedValueCdrc.readAndAdvanceNoThrow<PrfBlock>();
    if (!swEsc.isOK()) {
        return swEsc.getStatus();
    }

    auto swEcc = serverEncryptedValueCdrc.readAndAdvanceNoThrow<PrfBlock>();
    if (!swEcc.isOK()) {
        return swEcc.getStatus();
    }

    return FLE2IndexedEqualityEncryptedValue(
        EDCDerivedFromDataTokenAndContentionFactorToken(swEdc.getValue()),
        ESCDerivedFromDataTokenAndContentionFactorToken(swEsc.getValue()),
        ECCDerivedFromDataTokenAndContentionFactorToken(swEcc.getValue()),
        swCount.getValue(),
        type,
        indexKey,
        std::move(cipherText));
}


StatusWith<std::vector<uint8_t>> FLE2IndexedEqualityEncryptedValue::serialize(
    ServerDataEncryptionLevel1Token token) {
    BufBuilder builder(clientEncryptedValue.size() + sizeof(uint64_t) * 2 + sizeof(PrfBlock) * 3);


    builder.appendNum(LittleEndian<uint64_t>(clientEncryptedValue.size()));

    builder.appendBuf(clientEncryptedValue.data(), clientEncryptedValue.size());

    builder.appendNum(LittleEndian<uint64_t>(count));

    builder.appendStruct(edc.data);

    builder.appendStruct(esc.data);

    builder.appendStruct(ecc.data);

    dassert(builder.len() ==
            static_cast<int>(clientEncryptedValue.size() + sizeof(uint64_t) * 2 +
                             sizeof(PrfBlock) * 3));

    auto swEncryptedData = encryptData(token.toCDR(), ConstDataRange(builder.buf(), builder.len()));
    if (!swEncryptedData.isOK()) {
        return swEncryptedData;
    }

    auto cdrKeyId = indexKeyId.toCDR();
    auto serverEncryptedValue = swEncryptedData.getValue();

    std::vector<uint8_t> serializedServerValue(serverEncryptedValue.size() + cdrKeyId.length() + 1);

    std::copy(cdrKeyId.data(), cdrKeyId.data() + cdrKeyId.length(), serializedServerValue.begin());
    uint8_t bsonTypeByte = bsonType;
    std::copy(
        &bsonTypeByte, (&bsonTypeByte) + 1, serializedServerValue.begin() + cdrKeyId.length());
    std::copy(serverEncryptedValue.begin(),
              serverEncryptedValue.end(),
              serializedServerValue.begin() + cdrKeyId.length() + 1);

    return serializedServerValue;
}

ESCDerivedFromDataTokenAndContentionFactorToken EDCServerPayloadInfo::getESCToken() const {
    return FLETokenFromCDR<FLETokenType::ESCDerivedFromDataTokenAndContentionFactorToken>(
        payload.getEscDerivedToken());
}

std::vector<EDCServerPayloadInfo> EDCServerCollection::getEncryptedFieldInfo(BSONObj& obj) {
    std::vector<EDCServerPayloadInfo> fields;
    // TODO (SERVER-63736) - Validate only fields listed in EncryptedFieldConfig are indexed

    visitEncryptedBSON(obj, [&fields](ConstDataRange cdr, StringData fieldPath) {
        collectEDCServerInfo(&fields, cdr, fieldPath);
    });

    return fields;
}

PrfBlock EDCServerCollection::generateTag(EDCTwiceDerivedToken edcTwiceDerived, FLECounter count) {
    return prf(edcTwiceDerived.toCDR(), count);
}

PrfBlock EDCServerCollection::generateTag(const EDCServerPayloadInfo& payload) {
    auto token = FLETokenFromCDR<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>(
        payload.payload.getEdcDerivedToken());
    auto edcTwiceDerived = FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(token);
    return generateTag(edcTwiceDerived, payload.count);
}

BSONObj EDCServerCollection::finalizeForInsert(
    const BSONObj& doc, const std::vector<EDCServerPayloadInfo>& serverPayload) {

    std::vector<TagInfo> tags;
    // TODO - improve size estimate after range is supported since it no longer be 1 to 1
    tags.reserve(serverPayload.size());

    ConstVectorIteratorPair<EDCServerPayloadInfo> it(serverPayload);

    // First: transform all the markings
    auto obj = transformBSON(
        doc, [&tags, &it](ConstDataRange cdr, BSONObjBuilder* builder, StringData fieldPath) {
            convertServerPayload(&tags, it, builder, fieldPath);
        });

    BSONObjBuilder builder;

    // Second: reuse an existing array if present
    bool appendElements = true;
    for (const auto& element : obj) {
        if (element.fieldNameStringData() == kSafeContent) {
            uassert(6373510,
                    str::stream() << "Field '" << kSafeContent << "' was found but not an array",
                    element.type() == Array);
            BSONArrayBuilder subBuilder(builder.subarrayStart(kSafeContent));

            // Append existing array elements
            for (const auto& arrayElement : element.Obj()) {
                subBuilder.append(arrayElement);
            }

            // Add new tags
            for (auto const& tag : tags) {
                appendTag(tag.tag, &subBuilder);
            }

            appendElements = false;
        } else {
            builder.append(element);
        }
    }

    // Third: append the tags array if it does not exist
    if (appendElements) {
        BSONArrayBuilder subBuilder(builder.subarrayStart(kSafeContent));

        for (auto const& tag : tags) {
            appendTag(tag.tag, &subBuilder);
        }
    }

    return builder.obj();
}

std::vector<EDCIndexedFields> EDCServerCollection::getEncryptedIndexedFields(BSONObj& obj) {
    std::vector<EDCIndexedFields> fields;

    visitEncryptedBSON(obj, [&fields](ConstDataRange cdr, StringData fieldPath) {
        collectIndexedFields(&fields, cdr, fieldPath);
    });

    return fields;
}


BSONObj EncryptionInformationHelpers::encryptionInformationSerialize(NamespaceString& nss,
                                                                     EncryptedFieldConfig& ef) {
    EncryptionInformation ei;
    ei.setType(kEncryptionInformationSchemaVersion);

    ei.setSchema(BSON(nss.toString() << ef.toBSON()));

    return ei.toBSON();
}

EncryptedFieldConfig EncryptionInformationHelpers::getAndValidateSchema(
    const NamespaceString& nss, const EncryptionInformation& ei) {
    BSONObj schema = ei.getSchema();

    auto element = schema.getField(nss.toString());

    uassert(6371205,
            "Expected an object for schema in EncryptionInformation",
            !element.eoo() && element.type() == Object);

    auto efc = EncryptedFieldConfig::parse(IDLParserErrorContext("schema"), element.Obj());

    uassert(6371206, "Expected a value for eccCollection", efc.getEccCollection().has_value());
    uassert(6371207, "Expected a value for escCollection", efc.getEscCollection().has_value());
    uassert(6371208, "Expected a value for ecocCollection", efc.getEcocCollection().has_value());

    return efc;
}


}  // namespace mongo
