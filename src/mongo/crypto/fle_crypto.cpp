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
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/idl/basic_types.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"


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
constexpr auto kSafeContent = "__safeContent__";

using UUIDBuf = std::array<uint8_t, UUID::kNumBytes>;

static_assert(sizeof(PrfBlock) == SHA256Block::kHashLength);
static_assert(sizeof(KeyMaterial) == crypto::sym256KeySize);

PrfBlock blockToArray(SHA256Block& block) {
    PrfBlock data;
    memcpy(data.data(), block.data(), sizeof(PrfBlock));
    return data;
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

template <typename T>
T parseFromCDR(ConstDataRange cdr) {
    ConstDataRangeCursor cdc(cdr);
    auto obj = cdc.readAndAdvance<Validated<BSONObj>>();

    IDLParserErrorContext ctx("root");
    return T::parse(ctx, obj);
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

/**
 * AEAD AES + SHA256
 * Block size = 16 bytes
 * SHA-256 - block size = 256 bits = 32 bytes
 */
// TODO (SERVER-63382) - replace with call to CTR AEAD algorithm
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

StatusWith<std::vector<uint8_t>> encryptData(ConstDataRange key, ConstDataRange plainText) {

    return encryptDataWithAssociatedData(key, ConstDataRange(0, 0), plainText);
}

StatusWith<std::vector<uint8_t>> encryptData(ConstDataRange key, uint64_t value) {

    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);

    return encryptData(key, bufValue);
}

// TODO (SERVER-63382) - replace with call to CTR AEAD algorithm
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


StatusWith<std::vector<uint8_t>> decryptData(ConstDataRange key, ConstDataRange cipherText) {
    return decryptDataWithAssociatedData(key, ConstDataRange(0, 0), cipherText);
}

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


//#define DEBUG_ENUM_BINARY 1

template <typename collectionT, typename tagTokenT, typename valueTokenT>
uint64_t emuBinaryCommon(FLEStateCollectionReader* reader,
                         tagTokenT tagToken,
                         valueTokenT valueToken) {

    // Default search parameters
    uint64_t lambda = 0;
    uint64_t i = 0;

    // Step 2:
    // Search for null record
    PrfBlock nullRecordId = collectionT::generateId(tagToken, boost::none);

    BSONObj nullDoc = reader->getById(nullRecordId);

    if (!nullDoc.isEmpty()) {
        auto swNullEscDoc = collectionT::decryptNullDocument(valueToken, nullDoc);
        uassertStatusOK(swNullEscDoc.getStatus());
        lambda = swNullEscDoc.getValue().pos + 1;
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
    uint64_t maxIterations = ceil(log2(rho));

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


uint64_t ESCCollection::emuBinary(FLEStateCollectionReader* reader,
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

uint64_t ECCCollection::emuBinary(FLEStateCollectionReader* reader,
                                  ECCTwiceDerivedTagToken tagToken,
                                  ECCTwiceDerivedValueToken valueToken) {
    return emuBinaryCommon<ECCCollection, ECCTwiceDerivedTagToken, ECCTwiceDerivedValueToken>(
        reader, tagToken, valueToken);
}

}  // namespace mongo
