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

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/exception/exception.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_int/bitwise.hpp>
#include <boost/multiprecision/cpp_int/comparison.hpp>
#include <boost/multiprecision/cpp_int/divide.hpp>
#include <boost/multiprecision/cpp_int/limits.hpp>
#include <boost/multiprecision/cpp_int/literals.hpp>
#include <boost/multiprecision/cpp_int/multiply.hpp>
#include <boost/optional.hpp>
// IWYU pragma: no_include "boost/multiprecision/detail/default_ops.hpp"
// IWYU pragma: no_include "boost/multiprecision/detail/integer_ops.hpp"
// IWYU pragma: no_include "boost/multiprecision/detail/no_et_ops.hpp"
// IWYU pragma: no_include "boost/multiprecision/detail/number_base.hpp"
// IWYU pragma: no_include "boost/multiprecision/detail/number_compare.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/multiprecision/number.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <stack>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

extern "C" {
#include <mc-fle2-payload-iev-private-v2.h>
#include <mongocrypt-buffer-private.h>
#include <mongocrypt.h>
}

#include "mongo/base/data_builder.h"
#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_utf8.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/encryption_fields_validation.h"
#include "mongo/crypto/fle_crypto_predicate.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_fields_util.h"
#include "mongo/crypto/fle_numeric.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/crypto/fle_tokens_gen.h"
#include "mongo/crypto/fle_util.h"
#include "mongo/crypto/mongocryptbuffer.h"
#include "mongo/crypto/mongocryptstatus.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/wire_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

// Optional defines to help with debugging
//
// Appends unencrypted fields to the state collections to aid in debugging
// #define FLE2_DEBUG_STATE_COLLECTIONS

// Verbose std::cout to troubleshoot the EmuBinary algorithm
// #define DEBUG_ENUM_BINARY 1

#ifdef FLE2_DEBUG_STATE_COLLECTIONS
static_assert(kDebugBuild == 1, "Only use in debug builds");
#endif


namespace mongo {

namespace {

constexpr uint64_t kLevel1Collection = 1;
constexpr uint64_t kLevel1ServerTokenDerivation = 2;
constexpr uint64_t kLevelServerDataEncryption = 3;


constexpr uint64_t kEDC = 1;
constexpr uint64_t kESC = 2;
constexpr uint64_t kECOC = 4;


constexpr uint64_t kTwiceDerivedTokenFromEDC = 1;
constexpr uint64_t kTwiceDerivedTokenFromESCTag = 1;
constexpr uint64_t kTwiceDerivedTokenFromESCValue = 2;

constexpr uint64_t kServerCountAndContentionFactorEncryption = 1;
constexpr uint64_t kServerZerosEncryption = 2;

// "d" value in: S^esc_f_d = Fs[f,1,2,d]; where d = 17 octets of 0
constexpr char kAnchorPaddingTokenDVal[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static_assert(sizeof(kAnchorPaddingTokenDVal) == 17);

constexpr uint64_t kAnchorPaddingKeyToken = 1;
constexpr uint64_t kAnchorPaddingValueToken = 2;

constexpr int32_t kEncryptionInformationSchemaVersion = 1;

constexpr uint64_t kESCNullId = 0;
constexpr uint64_t kESCNonNullId = 1;

constexpr uint64_t KESCInsertRecordValue = 0;
constexpr uint64_t kESCompactionRecordValue = std::numeric_limits<uint64_t>::max();

constexpr uint64_t kESCAnchorId = 0;
constexpr uint64_t kESCNullAnchorPosition = 0;
constexpr uint64_t kESCNonNullAnchorValuePrefix = 0;
constexpr uint64_t kESCPaddingId = 0;

constexpr auto kId = "_id";
constexpr auto kValue = "value";
constexpr auto kFieldName = "fieldName";

constexpr auto kDollarPush = "$push";
constexpr auto kDollarPull = "$pull";
constexpr auto kDollarEach = "$each";
constexpr auto kDollarIn = "$in";

constexpr auto kEncryptedFields = "encryptedFields";

#ifdef FLE2_DEBUG_STATE_COLLECTIONS
constexpr auto kDebugId = "_debug_id";
constexpr auto kDebugValuePosition = "_debug_value_position";
constexpr auto kDebugValueCount = "_debug_value_count";

constexpr auto kDebugValueStart = "_debug_value_start";
constexpr auto kDebugValueEnd = "_debug_value_end";
#endif

using UniqueMongoCrypt =
    libmongocrypt_support_detail::libmongocrypt_unique_ptr<mongocrypt_t, mongocrypt_destroy>;

using UniqueMongoCryptCtx =
    libmongocrypt_support_detail::libmongocrypt_unique_ptr<mongocrypt_ctx_t,
                                                           mongocrypt_ctx_destroy>;

/**
 * C++ friendly wrapper around libmongocrypt's public mongocrypt_binary_t* and its associated
 * functions.
 *
 * mongocrypt_binary_t* is a view type. Callers must ensure the data has a valid lifetime.
 */
class MongoCryptBinary {
public:
    ~MongoCryptBinary() {
        mongocrypt_binary_destroy(_binary);
    }

    MongoCryptBinary(MongoCryptBinary&) = delete;
    MongoCryptBinary(MongoCryptBinary&&) = delete;

    static MongoCryptBinary create() {
        return MongoCryptBinary(mongocrypt_binary_new());
    }

    static MongoCryptBinary createFromCDR(ConstDataRange cdr) {
        return MongoCryptBinary(mongocrypt_binary_new_from_data(
            const_cast<uint8_t*>(cdr.data<uint8_t>()), cdr.length()));
    }

    static MongoCryptBinary createFromBSONObj(const BSONObj& obj) {
        return MongoCryptBinary(mongocrypt_binary_new_from_data(
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(obj.objdata())), obj.objsize()));
    }

    uint32_t length() {
        return mongocrypt_binary_len(_binary);
    }

    uint8_t* data() {
        return mongocrypt_binary_data(_binary);
    }

    ConstDataRange toCDR() {
        return ConstDataRange(data(), length());
    }

    /**
     * Callers responsibility to call getOwned() if needed as this is a view on underlying data
     */
    BSONObj toBSON() {
        return BSONObj(reinterpret_cast<const char*>(data()));
    }

    operator mongocrypt_binary_t*() {
        return _binary;
    }

private:
    explicit MongoCryptBinary(mongocrypt_binary_t* binary) : _binary(binary) {}

private:
    mongocrypt_binary_t* _binary;
};

using UUIDBuf = std::array<uint8_t, UUID::kNumBytes>;

static_assert(sizeof(PrfBlock) == SHA256Block::kHashLength);

ConstDataRange binDataToCDR(const BSONBinData binData) {
    int len = binData.length;
    const char* data = static_cast<const char*>(binData.data);
    return ConstDataRange(data, data + len);
}

ConstDataRange binDataToCDR(const Value& value) {
    uassert(6334103, "Expected binData Value type", value.getType() == BSONType::binData);

    return binDataToCDR(value.getBinData());
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

void toEncryptedBinDataPretyped(StringData field,
                                EncryptedBinDataType dt,
                                ConstDataRange cdr,
                                BSONObjBuilder* builder) {
    uassert(9784114, "Input buffer of encrypted data cannot be empty", cdr.length() > 0);
    auto dtAsNum = static_cast<uint8_t>(dt);
    auto firstByte = static_cast<uint8_t>(cdr.data()[0]);
    uassert(9588900,
            fmt::format(
                "Expected buffer to begin with type tag {}, but began with {}", dtAsNum, firstByte),
            firstByte == dtAsNum);

    builder->appendBinData(field, cdr.length(), BinDataType::Encrypt, cdr.data());
}

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedBinData(const BSONElement element) {
    uassert(
        6672414, "Expected binData with subtype Encrypt", element.isBinData(BinDataType::Encrypt));

    return fromEncryptedConstDataRange(binDataToCDR(element));
}

/**
 * AEAD AES + SHA256
 * Block size = 16 bytes
 * SHA-256 - block size = 256 bits = 32 bytes
 */
StatusWith<std::vector<uint8_t>> encryptDataWithAssociatedData(ConstDataRange key,
                                                               ConstDataRange associatedData,
                                                               ConstDataRange plainText,
                                                               crypto::aesMode mode) {
    std::vector<uint8_t> out(crypto::fle2AeadCipherOutputLength(plainText.length(), mode));

    auto k = key.slice(crypto::kFieldLevelEncryption2KeySize);
    auto status =
        crypto::fle2AeadEncrypt(k, plainText, ConstDataRange(0, 0), associatedData, out, mode);
    if (!status.isOK()) {
        return status;
    }

    return {out};
}

StatusWith<std::vector<uint8_t>> encryptData(ConstDataRange key, uint64_t value) {

    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);

    return FLEUtil::encryptData(key, bufValue);
}

StatusWith<std::vector<uint8_t>> decryptDataWithAssociatedData(ConstDataRange key,
                                                               ConstDataRange associatedData,
                                                               ConstDataRange cipherText,
                                                               crypto::aesMode mode) {
    auto swLen = fle2AeadGetMaximumPlainTextLength(cipherText.length());
    if (!swLen.isOK()) {
        return swLen.getStatus();
    }

    std::vector<uint8_t> out(swLen.getValue());

    auto k = key.slice(crypto::kFieldLevelEncryption2KeySize);
    auto swOutLen = crypto::fle2AeadDecrypt(k, cipherText, associatedData, out, mode);
    if (!swOutLen.isOK()) {
        return swOutLen.getStatus();
    }

    if (mode == crypto::aesMode::cbc) {
        // In CBC mode, the plaintext may end up shorter than the max possible
        // length because of padding, so the output buffer must be resized.
        out.resize(swOutLen.getValue());
    }

    return out;
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

template <typename T1, typename T2>
StatusWith<std::vector<uint8_t>> packAndEncrypt(std::tuple<T1, T2> tuple, const FLEToken& token) {
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
    return FLEUtil::encryptData(token.toCDR(), builder.getCursor());
}


template <typename T1, typename T2>
StatusWith<std::tuple<T1, T2>> decryptAndUnpack(ConstDataRange cdr, const FLEToken& token) {
    auto swVec = FLEUtil::decryptData(token.toCDR(), cdr);
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
                                                      ConstDataRange value,
                                                      crypto::aesMode mode = crypto::aesMode::cbc);
    /**
     * Read the key id from the payload.
     */
    static StatusWith<UUID> readKeyId(ConstDataRange cipherText);

    static StatusWith<std::vector<uint8_t>> decrypt(FLEUserKey userKey,
                                                    ConstDataRange cipherText,
                                                    crypto::aesMode mode = crypto::aesMode::cbc);
};

StatusWith<std::vector<uint8_t>> KeyIdAndValue::serialize(FLEUserKeyAndId userKey,
                                                          ConstDataRange value,
                                                          crypto::aesMode mode) {
    auto cdrKeyId = userKey.keyId.toCDR();

    auto swEncryptedData =
        encryptDataWithAssociatedData(userKey.key.toCDR(), cdrKeyId, value, mode);
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
                                                        ConstDataRange cipherText,
                                                        crypto::aesMode mode) {

    ConstDataRangeCursor baseCdrc(cipherText);

    auto swKeyId = baseCdrc.readAndAdvanceNoThrow<UUIDBuf>();
    if (!swKeyId.isOK()) {
        return {swKeyId.getStatus()};
    }

    UUID keyId = UUID::fromCDR(swKeyId.getValue());

    return decryptDataWithAssociatedData(userKey.toCDR(), keyId.toCDR(), baseCdrc, mode);
}

/**
 * Read and write FLE2InsertUpdate payload.
 */
class EDCClientPayload {
public:
    static FLE2InsertUpdatePayloadV2 parseInsertUpdatePayloadV2(ConstDataRange cdr);
    static FLE2InsertUpdatePayloadV2 serializeInsertUpdatePayloadV2(FLEIndexKeyAndId indexKey,
                                                                    FLEUserKeyAndId userKey,
                                                                    BSONElement element,
                                                                    uint64_t contentionFactor);
    static FLE2InsertUpdatePayloadV2 serializeInsertUpdatePayloadV2ForRange(
        FLEIndexKeyAndId indexKey,
        FLEUserKeyAndId userKey,
        FLE2RangeInsertSpec spec,
        uint8_t sparsity,
        uint64_t contentionFactor);
};

FLE2InsertUpdatePayloadV2 EDCClientPayload::parseInsertUpdatePayloadV2(ConstDataRange cdr) {
    return parseFromCDR<FLE2InsertUpdatePayloadV2>(cdr);
}

FLE2InsertUpdatePayloadV2 EDCClientPayload::serializeInsertUpdatePayloadV2(
    FLEIndexKeyAndId indexKey,
    FLEUserKeyAndId userKey,
    BSONElement element,
    uint64_t contentionFactor) {
    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = CollectionsLevel1Token::deriveFrom(indexKey.key);
    auto serverEncryptToken = ServerDataEncryptionLevel1Token::deriveFrom(indexKey.key);
    auto serverDerivationToken = ServerTokenDerivationLevel1Token::deriveFrom(indexKey.key);

    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto ecocToken = ECOCToken::deriveFrom(collectionToken);
    auto serverDerivedFromDataToken =
        ServerDerivedFromDataToken::deriveFrom(serverDerivationToken, value);
    EDCDerivedFromDataToken edcDataToken = EDCDerivedFromDataToken::deriveFrom(edcToken, value);
    ESCDerivedFromDataToken escDataToken = ESCDerivedFromDataToken::deriveFrom(escToken, value);

    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterToken =
        EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(edcDataToken, contentionFactor);
    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterToken =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDataToken, contentionFactor);

    FLE2InsertUpdatePayloadV2 iupayload;

    iupayload.setEncryptedTokens(
        StateCollectionTokensV2(escDataCounterToken, boost::none, boost::none).encrypt(ecocToken));

    iupayload.setEdcDerivedToken(std::move(edcDataCounterToken));
    iupayload.setEscDerivedToken(std::move(escDataCounterToken));
    iupayload.setServerEncryptionToken(std::move(serverEncryptToken));
    iupayload.setServerDerivedFromDataToken(std::move(serverDerivedFromDataToken));

    auto swCipherText = KeyIdAndValue::serialize(userKey, value);
    uassertStatusOK(swCipherText);
    iupayload.setValue(swCipherText.getValue());
    iupayload.setType(stdx::to_underlying(element.type()));
    iupayload.setIndexKeyId(indexKey.keyId);
    iupayload.setContentionFactor(contentionFactor);

    return iupayload;
}

std::unique_ptr<Edges> getEdges(FLE2RangeInsertSpec spec, int sparsity) {
    auto element = spec.getValue().getElement();
    auto minBound = spec.getMinBound().map([](IDLAnyType m) { return m.getElement(); });
    auto maxBound = spec.getMaxBound().map([](IDLAnyType m) { return m.getElement(); });
    auto trimFactor = spec.getTrimFactor();

    switch (element.type()) {
        case BSONType::numberInt:
            uassert(6775501,
                    "min bound must be integer",
                    !minBound.has_value() || minBound->type() == BSONType::numberInt);
            uassert(6775502,
                    "max bound must be integer",
                    !maxBound.has_value() || maxBound->type() == BSONType::numberInt);
            return getEdgesInt32(element.Int(),
                                 minBound.map([](BSONElement m) { return m.Int(); }),
                                 maxBound.map([](BSONElement m) { return m.Int(); }),
                                 sparsity,
                                 trimFactor);

        case BSONType::numberLong:
            uassert(6775503,
                    "min bound must be long int",
                    !minBound.has_value() || minBound->type() == BSONType::numberLong);
            uassert(6775504,
                    "max bound must be long int",
                    !maxBound.has_value() || maxBound->type() == BSONType::numberLong);
            return getEdgesInt64(element.Long(),
                                 minBound.map([](BSONElement m) { return int64_t(m.Long()); }),
                                 maxBound.map([](BSONElement m) { return int64_t(m.Long()); }),
                                 sparsity,
                                 trimFactor);

        case BSONType::date:
            uassert(6775505,
                    "min bound must be date",
                    !minBound.has_value() || minBound->type() == BSONType::date);
            uassert(6775506,
                    "max bound must be date",
                    !maxBound.has_value() || maxBound->type() == BSONType::date);
            return getEdgesInt64(element.Date().asInt64(),
                                 minBound.map([](BSONElement m) { return m.Date().asInt64(); }),
                                 maxBound.map([](BSONElement m) { return m.Date().asInt64(); }),
                                 sparsity,
                                 trimFactor);

        case BSONType::numberDouble:
            uassert(6775507,
                    "min bound must be double",
                    !minBound.has_value() || minBound->type() == BSONType::numberDouble);
            uassert(6775508,
                    "max bound must be double",
                    !maxBound.has_value() || maxBound->type() == BSONType::numberDouble);
            return getEdgesDouble(
                element.Double(),
                minBound.map([](BSONElement m) { return m.Double(); }),
                maxBound.map([](BSONElement m) { return m.Double(); }),
                spec.getPrecision().map([](std::int32_t m) { return static_cast<uint32_t>(m); }),
                sparsity,
                trimFactor);

        case BSONType::numberDecimal:
            uassert(6775509,
                    "min bound must be decimal",
                    !minBound.has_value() || minBound->type() == BSONType::numberDecimal);
            uassert(6775510,
                    "max bound must be decimal",
                    !maxBound.has_value() || maxBound->type() == BSONType::numberDecimal);
            return getEdgesDecimal128(
                element.numberDecimal(),
                minBound.map([](BSONElement m) { return m.numberDecimal(); }),
                maxBound.map([](BSONElement m) { return m.numberDecimal(); }),
                spec.getPrecision().map([](std::int32_t m) { return static_cast<uint32_t>(m); }),
                sparsity,
                trimFactor);

        default:
            uassert(6775500, "must use supported FLE2 range type", false);
    }
}

std::vector<EdgeTokenSetV2> getEdgeTokenSet(
    FLE2RangeInsertSpec spec,
    int sparsity,
    uint64_t contentionFactor,
    const EDCToken& edcToken,
    const ESCToken& escToken,
    const ECOCToken& ecocToken,
    const ServerTokenDerivationLevel1Token& serverDerivationToken) {
    const auto edges = getEdges(std::move(spec), sparsity);
    const auto edgesList = edges->get();

    std::vector<EdgeTokenSetV2> tokens;

    for (const auto& edge : edgesList) {
        ConstDataRange cdr(edge.data(), edge.size());

        EDCDerivedFromDataToken edcDatakey = EDCDerivedFromDataToken::deriveFrom(edcToken, cdr);
        ESCDerivedFromDataToken escDatakey = ESCDerivedFromDataToken::deriveFrom(escToken, cdr);

        EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
            EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(edcDatakey,
                                                                        contentionFactor);
        ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
            ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDatakey,
                                                                        contentionFactor);
        ServerDerivedFromDataToken serverDatakey =
            ServerDerivedFromDataToken::deriveFrom(serverDerivationToken, cdr);

        EdgeTokenSetV2 ets;

        const bool isLeaf = edge == edges->getLeaf();
        ets.setEncryptedTokens(
            StateCollectionTokensV2(escDataCounterkey, isLeaf, boost::none).encrypt(ecocToken));

        ets.setEdcDerivedToken(std::move(edcDataCounterkey));
        ets.setEscDerivedToken(std::move(escDataCounterkey));
        ets.setServerDerivedFromDataToken(std::move(serverDatakey));

        tokens.push_back(ets);
    }

    return tokens;
}


FLE2InsertUpdatePayloadV2 EDCClientPayload::serializeInsertUpdatePayloadV2ForRange(
    FLEIndexKeyAndId indexKey,
    FLEUserKeyAndId userKey,
    FLE2RangeInsertSpec spec,
    uint8_t sparsity,
    uint64_t contentionFactor) {
    auto element = spec.getValue().getElement();
    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = CollectionsLevel1Token::deriveFrom(indexKey.key);
    auto serverEncryptToken = ServerDataEncryptionLevel1Token::deriveFrom(indexKey.key);
    auto serverDerivationToken = ServerTokenDerivationLevel1Token::deriveFrom(indexKey.key);

    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto ecocToken = ECOCToken::deriveFrom(collectionToken);
    auto serverDerivedFromDataToken =
        ServerDerivedFromDataToken::deriveFrom(serverDerivationToken, value);

    EDCDerivedFromDataToken edcDatakey = EDCDerivedFromDataToken::deriveFrom(edcToken, value);
    ESCDerivedFromDataToken escDatakey = ESCDerivedFromDataToken::deriveFrom(escToken, value);

    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
        EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(edcDatakey, contentionFactor);
    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        ESCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(escDatakey, contentionFactor);

    FLE2InsertUpdatePayloadV2 iupayload;

    iupayload.setEncryptedTokens(
        StateCollectionTokensV2(escDataCounterkey, false /* isLeaf */, boost::none)
            .encrypt(ecocToken));

    iupayload.setEdcDerivedToken(std::move(edcDataCounterkey));
    iupayload.setEscDerivedToken(std::move(escDataCounterkey));
    iupayload.setServerEncryptionToken(std::move(serverEncryptToken));
    iupayload.setServerDerivedFromDataToken(std::move(serverDerivedFromDataToken));

    auto swCipherText = KeyIdAndValue::serialize(userKey, value);
    uassertStatusOK(swCipherText);
    iupayload.setValue(swCipherText.getValue());
    iupayload.setType(stdx::to_underlying(element.type()));
    iupayload.setIndexKeyId(indexKey.keyId);
    iupayload.setContentionFactor(contentionFactor);

    auto edgeTokenSet = getEdgeTokenSet(
        spec, sparsity, contentionFactor, edcToken, escToken, ecocToken, serverDerivationToken);

    iupayload.setSparsity(sparsity);
    iupayload.setPrecision(spec.getPrecision());
    iupayload.setTrimFactor(spec.getTrimFactor());
    iupayload.setIndexMin(spec.getMinBound());
    iupayload.setIndexMax(spec.getMaxBound());

    if (!edgeTokenSet.empty()) {
        iupayload.setEdgeTokenSet(edgeTokenSet);
    }

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
            if (elem.type() == BSONType::object) {
                frameStack.push({BSONObjIterator(elem.Obj()),
                                 BSONObjBuilder(builder.subobjStart(elem.fieldNameStringData()))});
            } else if (elem.type() == BSONType::array) {
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
            if (elem.type() == BSONType::object) {
                frameStack.emplace(
                    SinglyLinkedFieldPath(elem.fieldNameStringData(), &iterator.first),
                    BSONObjIterator(elem.Obj()));
            } else if (elem.type() == BSONType::array) {
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
void convertToFLE2Payload(FLEKeyVault* keyVault,
                          ConstDataRange cdr,
                          BSONObjBuilder* builder,
                          StringData fieldNameToSerialize,
                          const ContentionFactorFn& contentionFactor) {
    auto [encryptedType, subCdr] = fromEncryptedConstDataRange(cdr);

    if (encryptedType == EncryptedBinDataType::kFLE2Placeholder) {

        auto ep = parseFromCDR<FLE2EncryptionPlaceholder>(subCdr);

        auto el = ep.getValue().getElement();
        uassert(6409401,
                "Encrypting already encrypted data prohibited",
                !el.isBinData(BinDataType::Encrypt));

        FLEIndexKeyAndId indexKey = keyVault->getIndexKeyById(ep.getIndexKeyId());
        FLEUserKeyAndId userKey = keyVault->getUserKeyById(ep.getUserKeyId());

        if (ep.getAlgorithm() == Fle2AlgorithmInt::kEquality) {
            uassert(6338602,
                    str::stream() << "Type '" << typeName(el.type())
                                  << "' is not a valid type for Queryable Encryption Equality",
                    isFLE2EqualityIndexedSupportedType(el.type()));

            if (ep.getType() == Fle2PlaceholderType::kInsert) {

                auto iupayload = EDCClientPayload::serializeInsertUpdatePayloadV2(
                    indexKey, userKey, el, contentionFactor(ep));
                toEncryptedBinData(fieldNameToSerialize,
                                   EncryptedBinDataType::kFLE2InsertUpdatePayloadV2,
                                   iupayload,
                                   builder);

            } else if (ep.getType() == Fle2PlaceholderType::kFind) {

                auto findPayload = FLEClientCrypto::serializeFindPayloadV2(
                    indexKey, userKey, el, ep.getMaxContentionCounter());
                toEncryptedBinData(fieldNameToSerialize,
                                   EncryptedBinDataType::kFLE2FindEqualityPayloadV2,
                                   findPayload,
                                   builder);

            } else {
                uasserted(6410100, "Unsupported Queryable Encryption placeholder type");
            }
        } else if (ep.getAlgorithm() == Fle2AlgorithmInt::kRange) {

            if (ep.getType() == Fle2PlaceholderType::kInsert) {
                IDLParserContext ctx("root");
                auto rangeInsertSpec =
                    FLE2RangeInsertSpec::parse(ep.getValue().getElement().Obj(), ctx);

                auto elRange = rangeInsertSpec.getValue().getElement();

                uassert(6775301,
                        str::stream() << "Type '" << typeName(elRange.type())
                                      << "' is not a valid type for Queryable Encryption Range",
                        isFLE2RangeIndexedSupportedType(elRange.type()));

                auto iupayload = EDCClientPayload::serializeInsertUpdatePayloadV2ForRange(
                    indexKey,
                    userKey,
                    rangeInsertSpec,
                    ep.getSparsity().value(),  // Enforced as non-optional in this case in IDL
                    contentionFactor(ep));
                toEncryptedBinData(fieldNameToSerialize,
                                   EncryptedBinDataType::kFLE2InsertUpdatePayloadV2,
                                   iupayload,
                                   builder);

            } else if (ep.getType() == Fle2PlaceholderType::kFind) {
                IDLParserContext ctx("root");
                auto rangeFindSpec =
                    FLE2RangeFindSpec::parse(ep.getValue().getElement().Obj(), ctx);

                auto findPayload = [&]() {
                    if (rangeFindSpec.getEdgesInfo().has_value()) {
                        auto edges = getMinCover(rangeFindSpec, ep.getSparsity().value());

                        return FLEClientCrypto::serializeFindRangePayloadV2(
                            indexKey,
                            userKey,
                            std::move(edges),
                            ep.getMaxContentionCounter(),
                            ep.getSparsity()
                                .value(),  // Enforced as non-optional in this case in IDL
                            rangeFindSpec);
                    } else {
                        return FLEClientCrypto::serializeFindRangeStubV2(rangeFindSpec);
                    }
                }();

                toEncryptedBinData(fieldNameToSerialize,
                                   EncryptedBinDataType::kFLE2FindRangePayloadV2,
                                   findPayload,
                                   builder);

            } else {
                uasserted(6775303, "Unsupported Queryable Encryption placeholder type");
            }
        } else if (ep.getAlgorithm() == Fle2AlgorithmInt::kTextSearch) {
            uasserted(9978900,
                      "Queryable Encryption text search placeholder conversion must be done "
                      "through libmongocrypt");
        } else if (ep.getAlgorithm() == Fle2AlgorithmInt::kUnindexed) {
            uassert(6379102,
                    str::stream() << "Type '" << typeName(el.type())
                                  << "' is not a valid type for Queryable Encryption",
                    isFLE2UnindexedSupportedType(el.type()));
            uasserted(
                7133900,
                "Can't do FLE2UnindexedEncryptedValueV2 encryption in server. Use libmongocrypt");
        } else {
            uasserted(6338603,
                      "Only Queryable Encryption style encryption placeholders are supported");
        }


    } else {
        // TODO - validate acceptable types - kFLE2Placeholder or kFLE2UnindexedEncryptedValue or
        // kFLE2EqualityIndexedValue
        toEncryptedBinData(fieldNameToSerialize, encryptedType, subCdr, builder);
    }
}

void parseAndVerifyInsertUpdatePayload(std::vector<EDCServerPayloadInfo>* pFields,
                                       StringData fieldPath,
                                       EncryptedBinDataType type,
                                       ConstDataRange subCdr) {
    EDCServerPayloadInfo payloadInfo;
    payloadInfo.fieldPathName = std::string{fieldPath};

    uassert(7291901,
            "Encountered a Queryable Encryption insert/update payload type that is no "
            "longer supported",
            type == EncryptedBinDataType::kFLE2InsertUpdatePayloadV2);
    auto iupayload = EDCClientPayload::parseInsertUpdatePayloadV2(subCdr);
    payloadInfo.payload = std::move(iupayload);

    auto bsonType = static_cast<BSONType>(payloadInfo.payload.getType());

    uassert(9783801,
            "Queryable Encryption insert/update payload cannot have token sets for both range and "
            "text search types",
            !(payloadInfo.isRangePayload() && payloadInfo.isTextSearchPayload()));

    if (payloadInfo.isRangePayload()) {
        uassert(6775305,
                str::stream() << "Type '" << typeName(bsonType)
                              << "' is not a valid type for Queryable Encryption Range",
                isValidBSONType(payloadInfo.payload.getType()) &&
                    isFLE2RangeIndexedSupportedType(bsonType));
    } else if (payloadInfo.isTextSearchPayload()) {
        uassert(9783802,
                str::stream() << "Type '" << typeName(bsonType)
                              << "' is not a valid type for Queryable Encryption Text Search",
                isValidBSONType(payloadInfo.payload.getType()) && bsonType == BSONType::string);
    } else {
        uassert(6373504,
                str::stream() << "Type '" << typeName(bsonType)
                              << "' is not a valid type for Queryable Encryption Equality",
                isValidBSONType(payloadInfo.payload.getType()) &&
                    isFLE2EqualityIndexedSupportedType(bsonType));
    }

    pFields->push_back(std::move(payloadInfo));
}

void collectEDCServerInfo(std::vector<EDCServerPayloadInfo>* pFields,
                          ConstDataRange cdr,
                          StringData fieldPath) {

    // TODO - validate field is actually indexed in the schema?

    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
    auto encryptedType = encryptedTypeBinding;

    if (encryptedType == EncryptedBinDataType::kFLE2InsertUpdatePayload ||
        encryptedType == EncryptedBinDataType::kFLE2InsertUpdatePayloadV2) {
        parseAndVerifyInsertUpdatePayload(pFields, fieldPath, encryptedType, subCdr);
        return;
    } else if (encryptedType == EncryptedBinDataType::kFLE2FindEqualityPayload ||
               encryptedType == EncryptedBinDataType::kFLE2FindEqualityPayloadV2) {
        // No-op
        return;
    } else if (encryptedType == EncryptedBinDataType::kFLE2FindRangePayload ||
               encryptedType == EncryptedBinDataType::kFLE2FindRangePayloadV2) {
        // No-op
        return;
    } else if (encryptedType == EncryptedBinDataType::kFLE2UnindexedEncryptedValue ||
               encryptedType == EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2) {
        uassert(7413901,
                "Encountered a Queryable Encryption unindexed encrypted payload type that is "
                "no longer supported",
                encryptedType == EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2);
        return;
    }
    uasserted(6373503,
              str::stream() << "Unexpected encrypted payload type: "
                            << static_cast<uint32_t>(encryptedType));
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

void convertServerPayload(ConstDataRange cdr,
                          std::vector<TagInfo>* pTags,
                          ConstVectorIteratorPair<EDCServerPayloadInfo>& it,
                          BSONObjBuilder* builder,
                          StringData fieldPath) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
    if (encryptedTypeBinding == EncryptedBinDataType::kFLE2FindEqualityPayloadV2 ||
        encryptedTypeBinding == EncryptedBinDataType::kFLE2FindRangePayloadV2) {
        builder->appendBinData(fieldPath, cdr.length(), BinDataType::Encrypt, cdr.data<char>());
        return;
    } else if (encryptedTypeBinding == EncryptedBinDataType::kFLE2InsertUpdatePayloadV2) {
        uassert(6373505, "Unexpected end of iterator", it.it != it.end);
        const auto payload = it.it;

        // TODO - validate field is actually indexed in the schema?
        if (payload->isRangePayload()) {
            auto tags = EDCServerCollection::generateTagsForRange(*payload);
            auto swEncrypted = FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(
                                   payload->payload, tags, payload->counts)
                                   .serialize();
            uassertStatusOK(swEncrypted);
            toEncryptedBinDataPretyped(fieldPath,
                                       EncryptedBinDataType::kFLE2RangeIndexedValueV2,
                                       ConstDataRange(swEncrypted.getValue()),
                                       builder);

            for (auto& tag : tags) {
                pTags->push_back({tag});
            }
        } else if (payload->isTextSearchPayload()) {
            auto tags = EDCServerCollection::generateTagsForTextSearch(*payload);
            auto swEncrypted = FLE2IndexedTextEncryptedValue::fromUnencrypted(
                                   payload->payload, tags, payload->counts)
                                   .serialize();
            uassertStatusOK(swEncrypted);
            toEncryptedBinDataPretyped(fieldPath,
                                       EncryptedBinDataType::kFLE2TextIndexedValue,
                                       ConstDataRange(swEncrypted.getValue()),
                                       builder);
            for (auto& tag : tags) {
                pTags->push_back({tag});
            }
        } else {
            dassert(payload->counts.size() == 1);
            auto tag = EDCServerCollection::generateTag(*payload);
            auto swEncrypted = FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(
                                   payload->payload, tag, payload->counts[0])
                                   .serialize();
            uassertStatusOK(swEncrypted);

            toEncryptedBinDataPretyped(fieldPath,
                                       EncryptedBinDataType::kFLE2EqualityIndexedValueV2,
                                       ConstDataRange(swEncrypted.getValue()),
                                       builder);
            pTags->push_back({tag});
        }

    } else if (encryptedTypeBinding == EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2) {

        builder->appendBinData(fieldPath, cdr.length(), BinDataType::Encrypt, cdr.data());
        return;
    } else {
        uassert(6379103, "Unexpected type binding", false);
    }

    it.it++;
}

void collectIndexedFields(std::vector<EDCIndexedFields>* pFields,
                          ConstDataRange cdr,
                          StringData fieldPath) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);

    if (encryptedTypeBinding == EncryptedBinDataType::kFLE2EqualityIndexedValueV2 ||
        encryptedTypeBinding == EncryptedBinDataType::kFLE2RangeIndexedValueV2 ||
        encryptedTypeBinding == EncryptedBinDataType::kFLE2TextIndexedValue) {
        pFields->push_back({cdr, std::string{fieldPath}});
    }
}

void collectFieldValidationInfo(stdx::unordered_map<std::string, ConstDataRange>* pFields,
                                ConstDataRange cdr,
                                StringData fieldPath) {
    pFields->insert({std::string{fieldPath}, cdr});
}

stdx::unordered_map<std::string, EncryptedField> toFieldMap(const EncryptedFieldConfig& efc) {
    stdx::unordered_map<std::string, EncryptedField> fields;
    for (const auto& field : efc.getFields()) {
        fields.insert({std::string{field.getPath()}, field});
    }

    return fields;
}

uint64_t generateRandomContention(uint64_t cm) {
    // For non-contentious fields, we select the partition number, u, to be equal to 0.
    //
    // for contentious fields, with a contention factor, p, we pick the partition number, u,
    // uniformly at random from the set {0, ..., p}.
    //
    // Note: nextInt64() returns [0,p) instead of [0,p] so we +1.
    //
    uassert(6535701, "Illegal contention factor", cm != std::numeric_limits<uint64_t>::max());
    return cm > 0 ? SecureRandom().nextInt64(cm + 1) : 0;
}

size_t getEstimatedTagCount(const std::vector<EDCServerPayloadInfo>& serverPayload) {
    size_t total = 0;
    for (auto const& sp : serverPayload) {
        if (sp.isRangePayload()) {
            total += 1 + sp.payload.getEdgeTokenSet().get().size();
        } else if (sp.isTextSearchPayload()) {
            total += sp.getTotalTextSearchTokenSetCount();
        } else {
            total += 1;
        }
    }
    return total;
}

template <typename T>
std::string toBinaryString(T v) {
    static_assert(std::numeric_limits<T>::is_integer);
    static_assert(!std::numeric_limits<T>::is_signed);

    auto length = std::numeric_limits<T>::digits;
    std::string str(length, '0');

    const T kOne(1);

    for (size_t i = length; i > 0; i--) {
        T mask = kOne << (i - 1);
        if (v & mask) {
            str[length - i] = '1';
        }
    }

    return str;
}

void mongocryptLogHandler(mongocrypt_log_level_t level,
                          const char* message,
                          uint32_t message_len,
                          void* ctx) {
    switch (level) {
        case MONGOCRYPT_LOG_LEVEL_FATAL:
            LOGV2_FATAL(7132201, "libmongocrypt fatal error", "msg"_attr = message);
            break;
        case MONGOCRYPT_LOG_LEVEL_ERROR:
            LOGV2_ERROR(7132202, "libmongocrypt error", "msg"_attr = message);
            break;
        case MONGOCRYPT_LOG_LEVEL_WARNING:
            LOGV2_WARNING(7132203, "libmongocrypt warning", "msg"_attr = message);
            break;
        case MONGOCRYPT_LOG_LEVEL_INFO:
            LOGV2(7132204, "libmongocrypt info", "msg"_attr = message);
            break;
        case MONGOCRYPT_LOG_LEVEL_TRACE:
            LOGV2_DEBUG(7132205, 1, "libmongocrypt trace", "msg"_attr = message);
            break;
    }
}

void getKeys(mongocrypt_ctx_t* ctx, FLEKeyVault* keyVault) {
    MongoCryptBinary output = MongoCryptBinary::create();
    uassert(7132206, "mongocrypt_ctx_mongo_op failed", mongocrypt_ctx_mongo_op(ctx, output));

    BSONObj doc = output.toBSON();
    // Queries are of the shape:
    //    { $or: [
    //            { _id: { $in: [ UUID("12345678-1234-9876-1234-123456789012")
    //                      ]
    //                 }
    //            },
    //            {
    //            keyAltNames: {
    //                 $in: []
    //                 }
    //            }
    //       ]
    //  }
    auto orElement = doc["$or"];
    uassert(7132220, "failed to parse key query", !orElement.eoo());

    auto firstElement = orElement["0"];
    uassert(7132221, "failed to parse key query", !firstElement.eoo());

    auto idElement = firstElement["_id"];
    uassert(7132222, "failed to parse key query", !idElement.eoo());

    auto inElement = idElement["$in"];
    uassert(7132223, "failed to parse key query", !inElement.eoo());

    auto keyElements = inElement.Array();

    auto keys = std::vector<UUID>();
    std::transform(keyElements.begin(),
                   keyElements.end(),
                   std::back_inserter(keys),
                   [](auto element) { return UUID::fromCDR(element.uuid()); });

    for (auto& key : keys) {
        BSONObj keyDoc = keyVault->getEncryptedKey(key);
        auto input = MongoCryptBinary::createFromBSONObj(keyDoc);
        uassert(7132208, "mongocrypt_ctx_mongo_feed failed", mongocrypt_ctx_mongo_feed(ctx, input));
    }

    uassert(7132209, "mongocrypt_ctx_mongo_done failed", mongocrypt_ctx_mongo_done(ctx));
}

UniqueMongoCrypt createMongoCrypt() {
    UniqueMongoCrypt crypt(mongocrypt_new());

    mongocrypt_setopt_log_handler(crypt.get(), mongocryptLogHandler, nullptr);
    mongocrypt_setopt_enable_multiple_collinfo(crypt.get());

    return crypt;
}

BSONObj runStateMachineForEncryption(mongocrypt_ctx_t* ctx,
                                     FLEKeyVault* keyVault,
                                     const BSONObj& cryptdResult,
                                     StringData dbName) {
    bool done = false;
    BSONObj result;
    StringData errorContext = "encryptionStateMachine"_sd;

    while (!done) {
        switch (mongocrypt_ctx_state(ctx)) {
            case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS: {
                MongoCryptBinary opbin = MongoCryptBinary::create();
                if (!mongocrypt_ctx_mongo_op(ctx, opbin)) {
                    errorContext = "mongocrypt_ctx_mongo_op failed"_sd;
                    break;
                }

                BSONObj opobj = opbin.toBSON();

                bool feedOk = false;
                StringData opCmdName(opobj.firstElementFieldName());
                uassert(7132300,
                        "Invalid command obtained from mongocrypt_ctx_mongo_op",
                        !opCmdName.empty());
                if (str::equalCaseInsensitive(opCmdName, "isMaster")) {
                    BSONObjBuilder bob;
                    auto incomingExternalClient = WireSpec::getWireSpec(getGlobalServiceContext())
                                                      .getIncomingExternalClient();
                    bob.append("maxWireVersion", incomingExternalClient.maxWireVersion);
                    bob.append("minWireVersion", incomingExternalClient.minWireVersion);
                    auto reply = bob.done();
                    auto feed = MongoCryptBinary::createFromBSONObj(reply);
                    feedOk = mongocrypt_ctx_mongo_feed(ctx, feed);
                } else {
                    auto feed = MongoCryptBinary::createFromBSONObj(cryptdResult);
                    feedOk = mongocrypt_ctx_mongo_feed(ctx, feed);
                }

                if (!feedOk) {
                    errorContext = "mongocrypt_ctx_mongo_feed failed"_sd;
                } else if (!mongocrypt_ctx_mongo_done(ctx)) {
                    errorContext = "mongocrypt_ctx_mongo_done failed"_sd;
                }
                break;
            }
            case MONGOCRYPT_CTX_NEED_MONGO_KEYS: {
                getKeys(ctx, keyVault);
                break;
            }
            case MONGOCRYPT_CTX_READY: {
                MongoCryptBinary output = MongoCryptBinary::create();
                if (!mongocrypt_ctx_finalize(ctx, output)) {
                    errorContext = "mongocrypt_ctx_finalize failed"_sd;
                    break;
                }
                result = output.toBSON().getOwned();
                LOGV2_DEBUG(7132305,
                            1,
                            "Final command after transforming placeholders with libmongocrypt",
                            "cmd"_attr = result);
                break;
            }
            case MONGOCRYPT_CTX_DONE: {
                done = true;
                break;
            }
            case MONGOCRYPT_CTX_ERROR: {
                MongoCryptStatus status;
                mongocrypt_ctx_status(ctx, status);
                uassertStatusOK(status.toStatus().withContext(errorContext));
                break;
            }
            case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO: {
                // If this state is reached, it is because the command is a $lookup aggregation
                // where one or more namespaces referenced in the pipeline is an unencrypted
                // collection; and libmongocrypt, in turn, wants to obtain the collinfo for those
                // unencrypted namespaces. Since the namespace is unencrypted, we can provide
                // libmongocrypt with a listCollections reply without any collection options.
                // For FLE2 namespaces, we don't expect to be reached because mongocrypt_t should
                // already have the encrypted field config provided to it by the caller via
                // mongocrypt_setopt_encrypted_field_config_map().
                MongoCryptBinary opbin = MongoCryptBinary::create();
                if (!mongocrypt_ctx_mongo_op(ctx, opbin)) {
                    errorContext = "mongocrypt_ctx_mongo_op failed"_sd;
                    break;
                }

                // libmongocrypt supplies {name: <collection name>} filter for listCollections
                BSONObj opobj = opbin.toBSON();
                auto collName = opobj.getStringField("name");
                uassert(10128800,
                        "Invalid listCollections filter obtained from mongocrypt_ctx_mongo_op",
                        !collName.empty());

                BSONObjBuilder listCollectionReply;
                listCollectionReply.append("name", collName);
                listCollectionReply.append("type", "collection");
                auto feed = MongoCryptBinary::createFromBSONObj(listCollectionReply.done());
                auto feedOk = mongocrypt_ctx_mongo_feed(ctx, feed);
                if (!feedOk) {
                    errorContext = "mongocrypt_ctx_mongo_feed failed"_sd;
                } else if (!mongocrypt_ctx_mongo_done(ctx)) {
                    errorContext = "mongocrypt_ctx_mongo_done failed"_sd;
                }
                break;
            }
            case MONGOCRYPT_CTX_NEED_KMS: {
                // This state is responsible for decrypting data keys encrypted by a KMS. This is
                // not needed for local kms keys
                uasserted(7132302, "MONGOCRYPT_CTX_NEED_KMS not supported");
                break;
            }
            case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
                // We don't handle KMS credentials
                [[fallthrough]];
            default:
                uasserted(7132303, "unsupported state machine state");
                break;
        }
    }
    return result;
}

BSONObj runStateMachineForDecryption(mongocrypt_ctx_t* ctx, FLEKeyVault* keyVault) {
    mongocrypt_ctx_state_t state;
    bool done = false;
    BSONObj result;

    MongoCryptStatus status;

    while (!done) {
        state = mongocrypt_ctx_state(ctx);
        switch (state) {
            case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO:
                // This state is responsible for retrieving a collection schema from mongod. It is
                // not needed during decryption.
                uasserted(7132211, "MONGOCRYPT_CTX_NEED_MONGO_COLLINFO not supported");
                break;
            case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS:
                // This state is responsible for sending a document to mongocryptd/crypt_shared_v1
                // to be replace fields with encryption placeholders. It is not needed during
                // decryption.
                uasserted(7132212, "MONGOCRYPT_CTX_NEED_MONGO_MARKINGS not supported");
                break;
            case MONGOCRYPT_CTX_NEED_MONGO_KEYS: {
                getKeys(ctx, keyVault);
                break;
            }
            case MONGOCRYPT_CTX_NEED_KMS:
                // This state is responsible for decrypting data keys encrypted by a KMS. This is
                // not needed for local kms keys
                uasserted(7132213, "MONGOCRYPT_CTX_NEED_KMS not supported");
                break;
            case MONGOCRYPT_CTX_READY: {
                MongoCryptBinary output = MongoCryptBinary::create();
                uassert(7132214,
                        "mongocrypt_ctx_finalize failed",
                        mongocrypt_ctx_finalize(ctx, output));
                result = output.toBSON().getOwned();
                break;
            }
            case MONGOCRYPT_CTX_DONE: {
                done = true;
                break;
            }
            case MONGOCRYPT_CTX_ERROR: {
                mongocrypt_ctx_status(ctx, status);

                uassertStatusOK(status.toStatus().withContext("decryptionStateMachine"));
                break;
            }
            case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
                // We don't handle KMS credentials
                [[fallthrough]];
            default:
                uasserted(7132216, "unsupported state machine state");
                break;
        }
    }

    return result;
}

FLEEdgeCountInfo getEdgeCountInfoForPadding(HmacContext* hmacCtx,
                                            const FLEStateCollectionReader& reader,
                                            ConstDataRange tag) {
    auto anchorPaddingRootToken = AnchorPaddingRootToken::parse(tag);
    auto tagToken = AnchorPaddingKeyToken::deriveFrom(anchorPaddingRootToken);
    auto valueToken = AnchorPaddingValueToken::deriveFrom(anchorPaddingRootToken);
    // There are no non-anchor padding edges, so we can skip the binaryHops search.
    auto tracker = FLEStatusSection::get().makeEmuBinaryTracker();
    auto apos = ESCCollectionAnchorPadding::anchorBinaryHops(
        hmacCtx, reader, tagToken, valueToken, tracker);
    EmuBinaryResult positions{
        apos.value_or(1) > 0 ? boost::none : boost::make_optional<uint64_t>(0), apos};

    return ESCCollectionAnchorPadding::getEdgeCountInfoForPaddingCleanupCommon(
        hmacCtx, reader, tagToken, valueToken, positions);
}

FLEEdgeCountInfo getEdgeCountInfoForCleanup(HmacContext* hmacCtx,
                                            const FLEStateCollectionReader& reader,
                                            ConstDataRange tag) {
    auto escToken = EDCServerPayloadInfo::getESCToken(tag);
    auto tagToken = ESCTwiceDerivedTagToken::deriveFrom(escToken);
    auto valueToken = ESCTwiceDerivedValueToken::deriveFrom(escToken);
    auto positions = ESCCollection::emuBinaryV2(hmacCtx, reader, tagToken, valueToken);
    return ESCCollection::getEdgeCountInfoForPaddingCleanupCommon(
        hmacCtx, reader, tagToken, valueToken, positions);
}

/**
 * Performs all the ESC reads required by the QE compact algorithm.
 */
FLEEdgeCountInfo getEdgeCountInfoForCompact(HmacContext* hmacCtx,
                                            const FLEStateCollectionReader& reader,
                                            ConstDataRange tag) {
    auto escToken = EDCServerPayloadInfo::getESCToken(tag);

    auto tagToken = ESCTwiceDerivedTagToken::deriveFrom(escToken);
    auto valueToken = ESCTwiceDerivedValueToken::deriveFrom(escToken);

    auto positions = ESCCollection::emuBinaryV2(hmacCtx, reader, tagToken, valueToken);

    // Handle case where cpos is none. This means that no new non-anchors have been inserted
    // since since the last compact/cleanup.
    // This could happen if a previous compact inserted an anchor, but the temp ECOC drop
    // was interrupted. On restart, the compaction will run emuBinaryV2 again, but since the
    // anchor was already inserted for this value, it may return null cpos if there have been no
    // new insertions for that value since the first compact attempt.
    if (positions.cpos == boost::none) {
        // No new non-anchors since the last compact/cleanup.
        // There must be at least one anchor.
        uassert(7293602,
                "An ESC anchor document is expected but none is found",
                !positions.apos.has_value() || positions.apos.value() > 0);
        // the anchor with the latest cpos already exists so no more work needed

        return FLEEdgeCountInfo(
            0, tagToken.asPrfBlock(), positions, boost::none, reader.getStats(), boost::none);
    }

    uint64_t nextAnchorPos = 0;

    if (positions.apos == boost::none) {
        auto nullAnchorPositions = ESCCollection::readAndDecodeAnchor(
            reader, valueToken, ESCCollection::generateNullAnchorId(hmacCtx, tagToken));

        uassert(7293601, "ESC null anchor document not found", nullAnchorPositions);

        nextAnchorPos = nullAnchorPositions->apos + 1;
    } else {
        nextAnchorPos = positions.apos.value() + 1;
    }

    return FLEEdgeCountInfo(nextAnchorPos,
                            tagToken.asPrfBlock(),
                            positions,
                            boost::none,
                            reader.getStats(),
                            boost::none);
}

FLEEdgeCountInfo getEdgeCountInfo(HmacContext* hmacCtx,
                                  const FLEStateCollectionReader& reader,
                                  ConstDataRange tag,
                                  FLETagQueryInterface::TagQueryType type,
                                  const boost::optional<PrfBlock>& edc) {

    uint64_t count;

    auto escToken = EDCServerPayloadInfo::getESCToken(tag);

    auto tagToken = ESCTwiceDerivedTagToken::deriveFrom(escToken);
    auto valueToken = ESCTwiceDerivedValueToken::deriveFrom(escToken);

    auto positions = ESCCollection::emuBinaryV2(hmacCtx, reader, tagToken, valueToken);

    if (positions.cpos.has_value()) {
        // Either no ESC documents exist yet (cpos == 0), OR new non-anchors
        // have been inserted since the last compact/cleanup (cpos > 0).
        count = positions.cpos.value() + 1;
    } else {
        // No new non-anchors since the last compact/cleanup.
        // There must be at least one anchor.
        uassert(7291902,
                "An ESC anchor document is expected but none is found",
                !positions.apos.has_value() || positions.apos.value() > 0);

        PrfBlock anchorId;
        if (!positions.apos.has_value()) {
            anchorId = ESCCollection::generateNullAnchorId(hmacCtx, tagToken);
        } else {
            anchorId = ESCCollection::generateAnchorId(hmacCtx, tagToken, positions.apos.value());
        }

        auto anchorPositions = ESCCollection::readAndDecodeAnchor(reader, valueToken, anchorId);
        uassert(7291903, "ESC anchor document not found", anchorPositions);

        count = anchorPositions->cpos + 1;
    }


    if (type == FLETagQueryInterface::TagQueryType::kQuery) {
        count -= 1;
    }

    return FLEEdgeCountInfo(count, tagToken.asPrfBlock(), edc.map([](const PrfBlock& prf) {
        return EDCDerivedFromDataTokenAndContentionFactorToken::parse(prf);
    }));
}

}  // namespace

std::vector<std::string> getMinCover(const FLE2RangeFindSpec& spec, uint8_t sparsity) {
    uassert(7030000,
            "getMinCover should never be passed a findSpec without edges information",
            spec.getEdgesInfo());

    auto& edgesInfo = spec.getEdgesInfo().value();

    auto indexMin = edgesInfo.getIndexMin().getElement();
    auto indexMax = edgesInfo.getIndexMax().getElement();
    tassert(6901300, "Min and max must have the same type", indexMin.type() == indexMax.type());
    auto bsonType = indexMin.type();

    auto lowerBound = edgesInfo.getLowerBound().getElement();
    auto upperBound = edgesInfo.getUpperBound().getElement();
    auto includeLowerBound = edgesInfo.getLbIncluded();
    auto includeUpperBound = edgesInfo.getUbIncluded();

    auto trimFactor = edgesInfo.getTrimFactor();

    // Open-ended ranges are represented with infinity as the other endpoint. Resolve infinite
    // bounds at this point to end at the min or max for this index.
    if (isInfinite(lowerBound)) {
        lowerBound = indexMin;
        includeLowerBound = true;
    }
    if (isInfinite(upperBound)) {
        upperBound = indexMax;
        includeUpperBound = true;
    }

    // TODO: Check on the implications of safeNumberInt() and safeNumberLong().
    switch (bsonType) {
        case BSONType::numberInt:
            return minCoverInt32(lowerBound.safeNumberInt(),
                                 includeLowerBound,
                                 upperBound.safeNumberInt(),
                                 includeUpperBound,
                                 indexMin.Int(),
                                 indexMax.Int(),
                                 sparsity,
                                 trimFactor);
        case BSONType::numberLong:
            return minCoverInt64(lowerBound.safeNumberLong(),
                                 includeLowerBound,
                                 upperBound.safeNumberLong(),
                                 includeUpperBound,
                                 indexMin.Long(),
                                 indexMax.Long(),
                                 sparsity,
                                 trimFactor);
        case BSONType::date:
            return minCoverInt64(lowerBound.Date().asInt64(),
                                 includeLowerBound,
                                 upperBound.Date().asInt64(),
                                 includeUpperBound,
                                 indexMin.Date().asInt64(),
                                 indexMax.Date().asInt64(),
                                 sparsity,
                                 trimFactor);
        case BSONType::numberDouble:
            return minCoverDouble(lowerBound.numberDouble(),
                                  includeLowerBound,
                                  upperBound.numberDouble(),
                                  includeUpperBound,
                                  indexMin.numberDouble(),
                                  indexMax.numberDouble(),
                                  edgesInfo.getPrecision().map(
                                      [](std::int32_t m) { return static_cast<uint32_t>(m); }),
                                  sparsity,
                                  trimFactor);
        case BSONType::numberDecimal:
            return minCoverDecimal128(lowerBound.numberDecimal(),
                                      includeLowerBound,
                                      upperBound.numberDecimal(),
                                      includeUpperBound,
                                      indexMin.numberDecimal(),
                                      indexMax.numberDecimal(),
                                      edgesInfo.getPrecision().map(
                                          [](std::int32_t m) { return static_cast<uint32_t>(m); }),
                                      sparsity,
                                      trimFactor);
        default:
            // IDL validates that no other type is permitted.
            MONGO_UNREACHABLE_TASSERT(6901302);
    }
    MONGO_UNREACHABLE_TASSERT(6901303);
}

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedBinData(const Value& value) {
    uassert(6672416, "Expected binData with subtype Encrypt", value.getType() == BSONType::binData);

    auto binData = value.getBinData();

    uassert(6672415, "Expected binData with subtype Encrypt", binData.type == BinDataType::Encrypt);

    return fromEncryptedConstDataRange(binDataToCDR(binData));
}

boost::optional<EncryptedBinDataType> getEncryptedBinDataType(const Value& value) {
    if (value.getType() != BSONType::binData) {
        return boost::none;
    }
    auto binData = value.getBinData();
    if (binData.type != BinDataType::Encrypt || binData.length < 1) {
        return boost::none;
    }
    return static_cast<EncryptedBinDataType>(static_cast<const uint8_t*>(binData.data)[0]);
}

boost::optional<EncryptedBinDataType> getEncryptedBinDataType(const BSONElement& elt) {
    if (!elt.isBinData(BinDataType::Encrypt)) {
        return boost::none;
    }
    int dataLen;
    auto data = elt.binData(dataLen);
    if (dataLen < 1) {
        return boost::none;
    }
    return static_cast<EncryptedBinDataType>(data[0]);
}

BSONBinData toBSONBinData(const std::vector<uint8_t>& buf) {
    return BSONBinData(buf.data(), buf.size(), Encrypt);
}

std::vector<uint8_t> toEncryptedVector(EncryptedBinDataType dt, const PrfBlock& block) {
    std::vector<uint8_t> buf(block.size() + 1);
    buf[0] = static_cast<uint8_t>(dt);

    std::copy(block.data(), block.data() + block.size(), buf.data() + 1);

    return buf;
}

PrfBlock PrfBlockfromCDR(const ConstDataRange& block) {
    uassert(6373501, "Invalid prf length", block.length() == sizeof(PrfBlock));

    PrfBlock ret;
    std::copy(block.data(), block.data() + block.length(), ret.data());
    return ret;
}

std::vector<uint8_t> FLEUtil::vectorFromCDR(ConstDataRange cdr) {
    std::vector<uint8_t> buf(cdr.length());
    std::copy(cdr.data(), cdr.data() + cdr.length(), buf.data());
    return buf;
}

StatusWith<EncryptedStateCollectionTokens> EncryptedStateCollectionTokens::decryptAndParse(
    ECOCToken token, ConstDataRange cdr) {
    auto swUnpack = decryptAndUnpack<PrfBlock, PrfBlock>(cdr, token);

    if (!swUnpack.isOK()) {
        return swUnpack.getStatus();
    }

    auto value = swUnpack.getValue();

    return EncryptedStateCollectionTokens{
        ESCDerivedFromDataTokenAndContentionFactorToken(std::get<0>(value))};
}

StatusWith<std::vector<uint8_t>> EncryptedStateCollectionTokens::serialize(ECOCToken token) {
    constexpr uint64_t dummy{0};
    return packAndEncrypt(std::make_tuple(esc.asPrfBlock(), dummy), token);
}

StateCollectionTokensV2 StateCollectionTokensV2::Encrypted::decrypt(const ECOCToken& token) const
    try {
    assertLength(_encryptedTokens.size());
    const bool expectLeaf = _encryptedTokens.size() == kCipherLengthESCAndLeafFlag;
    const bool expectMsize = _encryptedTokens.size() == kCipherLengthESCAndMsize;

    auto data = uassertStatusOK(FLEUtil::decryptData(token.toCDR(), toCDR()));
    ConstDataRangeCursor cdrc(data);
    auto rawESCToken = cdrc.readAndAdvance<PrfBlock>();
    auto escToken = ESCDerivedFromDataTokenAndContentionFactorToken(std::move(rawESCToken));

    boost::optional<bool> isLeaf;
    boost::optional<uint32_t> msize;
    if (expectLeaf) {
        auto leaf = cdrc.readAndAdvance<uint8_t>();
        uassert(ErrorCodes::BadValue,
                fmt::format("Invalid value for ESCTokensV2 leaf tag {}", leaf),
                (leaf == 0) || (leaf == 1));

        isLeaf = !!leaf;
    }

    if (expectMsize) {
        // Read the 3 byte LE msize, and convert it to a uint32_t.
        auto msize_arr = cdrc.readAndAdvance<std::array<std::uint8_t, 3>>();
        msize = msize_arr[0] | (msize_arr[1] << 8) | (msize_arr[2] << 16);
    }

    return StateCollectionTokensV2(std::move(escToken), std::move(isLeaf), std::move(msize));

} catch (const DBException& ex) {
    uassertStatusOK(ex.toStatus().withContext("Failed decrypting StateCollectionTokensV2"));
    MONGO_UNREACHABLE;
}

StateCollectionTokensV2::Encrypted StateCollectionTokensV2::encrypt(const ECOCToken& token) const
    try {
    std::vector<std::uint8_t> encryptedTokens;
    if (isRange()) {
        DataBuilder builder(sizeof(PrfBlock) + 1);
        uassertStatusOK(builder.writeAndAdvance(_esc.toCDR()));
        uassertStatusOK(builder.writeAndAdvance(*_isLeaf));
        encryptedTokens = uassertStatusOK(FLEUtil::encryptData(token.toCDR(), builder.getCursor()));
    } else if (isTextSearch()) {
        DataBuilder builder(sizeof(PrfBlock) + 3);
        uassertStatusOK(builder.writeAndAdvance(_esc.toCDR()));
        // Write msize as a little-endian 3-byte integer.
        uassertStatusOK(builder.writeAndAdvance(
            std::array<uint8_t, 3>{(std::uint8_t)(*_msize & 0xFF),
                                   (std::uint8_t)((*_msize >> 8) & 0xFF),
                                   (std::uint8_t)((*_msize >> 16) & 0xFF)}));
        encryptedTokens = uassertStatusOK(FLEUtil::encryptData(token.toCDR(), builder.getCursor()));
    } else {
        // Equality
        encryptedTokens = uassertStatusOK(FLEUtil::encryptData(token.toCDR(), _esc.toCDR()));
    }

    return StateCollectionTokensV2::Encrypted(std::move(encryptedTokens));
} catch (const DBException& ex) {
    uassertStatusOK(ex.toStatus().withContext("Failed encrypting StateCollectionTokensV2"));
    MONGO_UNREACHABLE;
}

BSONObj StateCollectionTokensV2::Encrypted::generateDocument(StringData fieldName) const {
    assertLength(_encryptedTokens.size());
    BSONObjBuilder builder;
    builder.append(kId, OID::gen());
    builder.append(kFieldName, fieldName);
    toBinData(kValue, toCDR(), &builder);
    return builder.obj();
}

FLEKeyVault::~FLEKeyVault() {}

FLETagQueryInterface::~FLETagQueryInterface() {}

BSONObj FLEClientCrypto::transformPlaceholders(const BSONObj& obj, FLEKeyVault* keyVault) {
    return transformPlaceholders(obj, keyVault, [](const FLE2EncryptionPlaceholder& ep) {
        // Generate a number between [1,maxContentionFactor]
        return generateRandomContention(ep.getMaxContentionCounter());
    });
}

BSONObj FLEClientCrypto::transformPlaceholders(const BSONObj& obj,
                                               FLEKeyVault* keyVault,
                                               const ContentionFactorFn& cf) {
    auto ret = transformBSON(
        obj, [keyVault, cf](ConstDataRange cdr, BSONObjBuilder* builder, StringData field) {
            convertToFLE2Payload(keyVault, cdr, builder, field, cf);
        });

    return ret;
}

BSONObj FLEClientCrypto::transformPlaceholders(const BSONObj& originalCmd,
                                               const BSONObj& cryptdResult,
                                               const BSONObj& encryptedFieldConfigMap,
                                               FLEKeyVault* keyVault,
                                               StringData dbName) {
    auto crypt = createMongoCrypt();
    LOGV2_DEBUG(7132304,
                1,
                "Transforming placeholders with libmongocrypt",
                "originalCmd"_attr = originalCmd,
                "cryptdResult"_attr = cryptdResult);

    auto uassertMongoCryptStatusOK = [](mongocrypt_t* crypt, bool result, StringData context) {
        if (!result) {
            MongoCryptStatus status;
            mongocrypt_status(crypt, status);
            uassertStatusOK(status.toStatus().withContext(context));
        }
    };

    {
        SymmetricKey& key = keyVault->getKMSLocalKey();
        auto binary =
            MongoCryptBinary::createFromCDR(ConstDataRange(key.getKey(), key.getKeySize()));
        auto efcMap = MongoCryptBinary::createFromBSONObj(encryptedFieldConfigMap);
        uassertMongoCryptStatusOK(crypt.get(),
                                  mongocrypt_setopt_kms_provider_local(crypt.get(), binary),
                                  "mongocrypt_setopt_kms_provider_local failed");
        uassertMongoCryptStatusOK(crypt.get(),
                                  mongocrypt_setopt_encrypted_field_config_map(crypt.get(), efcMap),
                                  "mongocrypt_setopt_encrypted_field_config_map failed");
    }

    uassertMongoCryptStatusOK(crypt.get(), mongocrypt_init(crypt.get()), "mongocrypt_init failed");

    auto cmdBin = MongoCryptBinary::createFromBSONObj(originalCmd);
    UniqueMongoCryptCtx ctx(mongocrypt_ctx_new(crypt.get()));
    if (!mongocrypt_ctx_encrypt_init(ctx.get(), dbName.data(), dbName.length(), cmdBin)) {
        MongoCryptStatus status;
        mongocrypt_ctx_status(ctx.get(), status);
        uassertStatusOK(status.toStatus().withContext("mongocrypt_ctx_encrypt_init failed"));
    }

    return runStateMachineForEncryption(ctx.get(), keyVault, cryptdResult, dbName);
}

BSONObj FLEClientCrypto::generateCompactionTokens(const EncryptedFieldConfig& cfg,
                                                  FLEKeyVault* keyVault) {
    BSONObjBuilder tokensBuilder;
    auto& fields = cfg.getFields();
    for (const auto& field : fields) {
        auto indexKey = keyVault->getIndexKeyById(field.getKeyId());
        auto collToken = CollectionsLevel1Token::deriveFrom(indexKey.key);
        auto ecocToken = ECOCToken::deriveFrom(collToken);
        auto tokenCdr = ecocToken.toCDR();
        if (hasQueryTypeMatching(field, [](QueryTypeEnum type) {
                return type == QueryTypeEnum::RangePreviewDeprecated ||
                    type == QueryTypeEnum::Range || isFLE2TextQueryType(type);
            })) {
            BSONObjBuilder token(tokensBuilder.subobjStart(field.getPath()));
            token.appendBinData(CompactionTokenDoc::kECOCTokenFieldName,
                                tokenCdr.length(),
                                BinDataType::BinDataGeneral,
                                tokenCdr.data());

            auto escToken = ESCToken::deriveFrom(collToken);
            auto paddingToken = AnchorPaddingRootToken::deriveFrom(escToken);
            auto paddingCdr = paddingToken.toCDR();
            token.appendBinData(CompactionTokenDoc::kAnchorPaddingTokenFieldName,
                                paddingCdr.length(),
                                BinDataType::BinDataGeneral,
                                paddingCdr.data());
        } else {
            // Equality
            tokensBuilder.appendBinData(
                field.getPath(), tokenCdr.length(), BinDataType::BinDataGeneral, tokenCdr.data());
        }
    }
    return tokensBuilder.obj();
}

BSONObj FLEClientCrypto::decryptDocument(const BSONObj& doc, FLEKeyVault* keyVault) {
    auto crypt = createMongoCrypt();

    SymmetricKey& key = keyVault->getKMSLocalKey();
    auto binary = MongoCryptBinary::createFromCDR(ConstDataRange(key.getKey(), key.getKeySize()));
    uassert(7132217,
            "mongocrypt_setopt_kms_provider_local failed",
            mongocrypt_setopt_kms_provider_local(crypt.get(), binary));

    uassert(7132218, "mongocrypt_init failed", mongocrypt_init(crypt.get()));

    UniqueMongoCryptCtx ctx(mongocrypt_ctx_new(crypt.get()));
    auto input = MongoCryptBinary::createFromBSONObj(doc);
    mongocrypt_ctx_decrypt_init(ctx.get(), input);
    BSONObj obj = runStateMachineForDecryption(ctx.get(), keyVault);

    return obj;
}

void FLEClientCrypto::validateTagsArray(const BSONObj& doc) {
    BSONElement safeContent = doc[kSafeContent];

    uassert(6371506,
            str::stream() << "Found indexed encrypted fields but could not find " << kSafeContent,
            !safeContent.eoo());

    uassert(6371507,
            str::stream() << kSafeContent << " must be an array",
            safeContent.type() == BSONType::array);
}

PrfBlock ESCCollection::generateId(HmacContext* context,
                                   const ESCTwiceDerivedTagToken& tagToken,
                                   boost::optional<uint64_t> index) {
    if (index.has_value()) {
        return FLEUtil::prf(context, tagToken.toCDR(), kESCNonNullId, index.value());
    } else {
        return FLEUtil::prf(context, tagToken.toCDR(), kESCNullId, 0);
    }
}

PrfBlock ESCCollection::generateNonAnchorId(HmacContext* context,
                                            const ESCTwiceDerivedTagToken& tagToken,
                                            uint64_t cpos) {
    return FLEUtil::prf(context, tagToken.toCDR(), cpos);
}

template <class TagToken, class ValueToken>
PrfBlock ESCCollectionCommon<TagToken, ValueToken>::generateAnchorId(HmacContext* context,
                                                                     const TagToken& tagToken,
                                                                     uint64_t apos) {
    return FLEUtil::prf(context, tagToken.toCDR(), kESCAnchorId, apos);
}

template <class TagToken, class ValueToken>
PrfBlock ESCCollectionCommon<TagToken, ValueToken>::generateNullAnchorId(HmacContext* hmacCtx,
                                                                         const TagToken& tagToken) {
    return generateAnchorId(hmacCtx, tagToken, kESCNullAnchorPosition);
}

template <class TagToken, class ValueToken>
boost::optional<ESCCountsPair> ESCCollectionCommon<TagToken, ValueToken>::readAndDecodeAnchor(
    const FLEStateCollectionReader& reader,
    const ValueToken& valueToken,
    const PrfBlock& anchorId) {
    auto anchor = reader.getById(anchorId);
    if (anchor.isEmpty()) {
        return boost::none;
    }

    auto anchorDoc = uassertStatusOK(decryptAnchorDocument(valueToken, anchor));
    ESCCountsPair positions;
    positions.apos = anchorDoc.position;
    positions.cpos = anchorDoc.count;
    return positions;
}

template <class TagToken, class ValueToken>
FLEEdgeCountInfo ESCCollectionCommon<TagToken, ValueToken>::getEdgeCountInfoForPaddingCleanupCommon(
    HmacContext* hmacCtx,
    const FLEStateCollectionReader& reader,
    const TagToken& tagToken,
    const ValueToken& valueToken,
    const EmuBinaryResult& positions) {
    // step (D)
    // nullAnchorPositions is r
    auto nullAnchorPositions =
        readAndDecodeAnchor(reader, valueToken, generateNullAnchorId(hmacCtx, tagToken));

    // This holds what value of a_1 should be used when inserting/updating the null anchor.
    auto latestCpos = 0;

    if (positions.apos == boost::none) {
        // case (E)
        // Null anchor exists & contains the latest anchor position,
        // and *maybe* the latest non-anchor position.
        uassert(7295004, "ESC null anchor is expected but not found", nullAnchorPositions);

        // emuBinary must not return 0 for cpos if an anchor exists
        uassert(7295005, "Invalid non-anchor position encountered", positions.cpos.value_or(1) > 0);

        // If emuBinary returns none for a_1, then the null anchor has the latest non-anchor pos.
        // This may happen if a prior cleanup was interrupted after the null anchors were updated,
        // but before the ECOC temp collection could be dropped, and on resume, no new insertions
        // or compactions have occurred since the previous cleanup.
        latestCpos = positions.cpos.value_or(nullAnchorPositions->cpos);

    } else if (positions.apos.value() == 0) {
        // case (F)
        // No anchors yet exist, so null anchor cannot exist and emuBinary must have
        // returned a value for cpos.
        uassert(7295006, "Unexpected ESC null anchor is found", !nullAnchorPositions);
        uassert(7295007, "Invalid non-anchor position encountered", positions.cpos);

        latestCpos = positions.cpos.value();
    } else /* (apos > 0) */ {
        // case (G)
        // New anchors exist - if null anchor exists, then it contains stale positions.

        // emuBinary must not return 0 for cpos if an anchor exists
        uassert(7295008, "Invalid non-anchor position encountered", positions.cpos.value_or(1) > 0);

        // If emuBinary returns none for cpos, then the newest anchor has the latest non-anchor pos.
        // This may happen if a prior compact was interrupted after it inserted a new anchor, but
        // before the ECOC temp collection could be dropped, and cleanup started immediately
        // after.
        latestCpos = positions.cpos.value_or_eval([&]() {
            auto anchorPositions = readAndDecodeAnchor(
                reader, valueToken, generateAnchorId(hmacCtx, tagToken, positions.apos.value()));
            uassert(7295009, "ESC anchor is expected but not found", anchorPositions);
            return anchorPositions->cpos;
        });
    }

    return FLEEdgeCountInfo(latestCpos,
                            tagToken.asPrfBlock(),
                            positions,
                            nullAnchorPositions,
                            reader.getStats(),
                            boost::none);
}

BSONObj ESCCollection::generateNullDocument(HmacContext* hmacCtx,
                                            const ESCTwiceDerivedTagToken& tagToken,
                                            const ESCTwiceDerivedValueToken& valueToken,
                                            uint64_t pos,
                                            uint64_t count) {
    auto block = generateId(hmacCtx, tagToken, boost::none);

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

PrfBlock ESCCollectionAnchorPadding::generateAnchorId(HmacContext* context,
                                                      const AnchorPaddingKeyToken& keyToken,
                                                      uint64_t apos) {
    return FLEUtil::prf(context, keyToken.toCDR(), kESCPaddingId, apos);
}

BSONObj ESCCollectionAnchorPadding::generatePaddingDocument(
    HmacContext* hmacCtx,
    const AnchorPaddingKeyToken& keyToken,
    const AnchorPaddingValueToken& valueToken,
    uint64_t id) {
    auto block = generateAnchorId(hmacCtx, keyToken, id);

    constexpr uint64_t dummy{0};
    auto cipherText = uassertStatusOK(packAndEncrypt(std::tie(dummy, dummy), valueToken));

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, cipherText, &builder);
#ifdef FLE2_DEBUG_STATE_COLLECTIONS
    builder.append(kDebugId, fmt::format("NULL DOC({})", id));
    builder.append(kDebugValuePosition, 0);
    builder.append(kDebugValueCount, 0);
#endif

    return builder.obj();
}

BSONObj ESCCollection::generateInsertDocument(HmacContext* hmacCtx,
                                              const ESCTwiceDerivedTagToken& tagToken,
                                              const ESCTwiceDerivedValueToken& valueToken,
                                              uint64_t index,
                                              uint64_t count) {
    auto block = generateId(hmacCtx, tagToken, index);

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

BSONObj ESCCollection::generateCompactionPlaceholderDocument(
    HmacContext* hmacCtx,
    const ESCTwiceDerivedTagToken& tagToken,
    const ESCTwiceDerivedValueToken& valueToken,
    uint64_t index,
    uint64_t count) {
    auto block = generateId(hmacCtx, tagToken, index);

    auto swCipherText = packAndEncrypt(std::tie(kESCompactionRecordValue, count), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);

    return builder.obj();
}

BSONObj ESCCollection::generateNonAnchorDocument(HmacContext* hmacCtx,
                                                 const ESCTwiceDerivedTagToken& tagToken,
                                                 uint64_t cpos) {
    auto block = generateNonAnchorId(hmacCtx, tagToken, cpos);
    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    return builder.obj();
}

BSONObj ESCCollection::generateAnchorDocument(HmacContext* hmacCtx,
                                              const ESCTwiceDerivedTagToken& tagToken,
                                              const ESCTwiceDerivedValueToken& valueToken,
                                              uint64_t apos,
                                              uint64_t cpos) {
    auto block = generateAnchorId(hmacCtx, tagToken, apos);

    auto swCipherText = packAndEncrypt(std::tie(kESCNonNullAnchorValuePrefix, cpos), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
    return builder.obj();
}

BSONObj ESCCollection::generateNullAnchorDocument(HmacContext* hmacCtx,
                                                  const ESCTwiceDerivedTagToken& tagToken,
                                                  const ESCTwiceDerivedValueToken& valueToken,
                                                  uint64_t apos,
                                                  uint64_t cpos) {
    auto block = generateNullAnchorId(hmacCtx, tagToken);

    auto swCipherText = packAndEncrypt(std::tie(apos, cpos), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
    return builder.obj();
}

PrfBlock ESCCollectionAnchorPadding::generateNullAnchorId(HmacContext* hmacCtx,
                                                          const AnchorPaddingKeyToken& keyToken) {
    return FLEUtil::prf(hmacCtx, keyToken.toCDR(), kESCPaddingId, 0);
}

BSONObj ESCCollectionAnchorPadding::generateNullAnchorDocument(
    HmacContext* hmacCtx,
    const AnchorPaddingKeyToken& keyToken,
    const AnchorPaddingValueToken& valueToken,
    uint64_t apos,
    uint64_t /* cpos */) {
    auto block = generateNullAnchorId(hmacCtx, keyToken);

    constexpr uint64_t ignored{0};
    auto cipherText = uassertStatusOK(packAndEncrypt(std::tie(apos, ignored), valueToken));

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, cipherText, &builder);
    return builder.obj();
}

StatusWith<ESCNullDocument> ESCCollection::decryptNullDocument(
    const ESCTwiceDerivedValueToken& valueToken, const BSONObj& doc) {
    BSONElement encryptedValue;
    auto status = bsonExtractTypedField(doc, kValue, BSONType::binData, &encryptedValue);
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

template <class TagToken, class ValueToken>
StatusWith<ESCDocument> ESCCollectionCommon<TagToken, ValueToken>::decryptDocument(
    const ValueToken& valueToken, const BSONObj& doc) {
    BSONElement encryptedValue;
    auto status = bsonExtractTypedField(doc, kValue, BSONType::binData, &encryptedValue);
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

template <class TagToken, class ValueToken>
StatusWith<ESCDocument> ESCCollectionCommon<TagToken, ValueToken>::decryptAnchorDocument(
    const ValueToken& valueToken, const BSONObj& doc) {
    return decryptDocument(valueToken, doc);
}

namespace {
boost::optional<uint64_t> binarySearchCommon(const FLEStateCollectionReader& reader,
                                             uint64_t rho,
                                             uint64_t lambda,
                                             boost::optional<uint64_t> i,
                                             std::function<PrfBlock(uint64_t)> idGenerator,
                                             FLEStatusSection::EmuBinaryTracker& tracker) {

    bool flag = true;
    while (flag) {
        bool docExists = reader.existsById(idGenerator(rho + lambda));

#ifdef DEBUG_ENUM_BINARY
        std::cout << fmt::format("search1: rho: {},  doc: {}", rho, doc.toString()) << std::endl;
#endif
        if (docExists) {
            rho = 2 * rho;
        } else {
            flag = false;
        }
    }

    uint64_t median = 0, min = 1, max = rho;
    uint64_t maxIterations = ceil(log2(rho));

    for (uint64_t j = 1; j <= maxIterations; j++) {
        tracker.recordSuboperation();
        median = ceil(static_cast<double>(max - min) / 2) + min;

        bool docExists = reader.existsById(idGenerator(median + lambda));

#ifdef DEBUG_ENUM_BINARY
        std::cout << fmt::format("search_stat: min: {}, median: {}, max: {}, i: {}, doc: {}",
                                 min,
                                 median,
                                 max,
                                 i,
                                 doc.toString())
                  << std::endl;
#endif

        if (docExists) {
            min = median;
            if (j == maxIterations) {
                i = min + lambda;
            }
        } else {
            max = median;

            // Binary search has ended without finding a document, check for the first document
            // explicitly
            if (j == maxIterations && min == 1) {
                bool docExists2 = reader.existsById(idGenerator(1 + lambda));
                if (docExists2) {
                    i = 1 + lambda;
                }
            } else if (j == maxIterations && min != 1) {
                i = min + lambda;
            }
        }
    }

    return i;
}
}  // namespace

EmuBinaryResult ESCCollection::emuBinaryV2(HmacContext* context,
                                           const FLEStateCollectionReader& reader,
                                           const ESCTwiceDerivedTagToken& tagToken,
                                           const ESCTwiceDerivedValueToken& valueToken) {
    auto tracker = FLEStatusSection::get().makeEmuBinaryTracker();

    context->setReuseKey(true);
    auto x = anchorBinaryHops(context, reader, tagToken, valueToken, tracker);
    context->resetCount();
    auto i = binaryHops(context, reader, tagToken, valueToken, x, tracker);
    context->setReuseKey(false);
    return EmuBinaryResult{i, x};
}

template <class TagToken, class ValueToken>
boost::optional<uint64_t> ESCCollectionCommon<TagToken, ValueToken>::anchorBinaryHops(
    HmacContext* context,
    const FLEStateCollectionReader& reader,
    const TagToken& tagToken,
    const ValueToken& valueToken,
    FLEStatusSection::EmuBinaryTracker& tracker) {

    uint64_t lambda;
    boost::optional<uint64_t> x;

    // 1. find null anchor
    PrfBlock nullAnchorId = generateNullAnchorId(context, tagToken);
    BSONObj nullAnchorDoc = reader.getById(nullAnchorId);

    // 2. case: null anchor exists
    if (!nullAnchorDoc.isEmpty()) {
        auto swAnchor = decryptDocument(valueToken, nullAnchorDoc);
        uassertStatusOK(swAnchor.getStatus());
        lambda = swAnchor.getValue().position;
        x = boost::none;
    }
    // 3. case: null anchor does not exist
    else {
        lambda = 0;
        x = 0;
    }

    // 4. initialize rho at 2
    uint64_t rho = 2;

    // 5-8. perform binary searches
    auto idGenerator = [context, &tagToken](uint64_t value) -> PrfBlock {
        return generateAnchorId(context, tagToken, value);
    };

#ifdef DEBUG_ENUM_BINARY
    std::cout << fmt::format(
                     "anchor binary search start: lambda: {}, i: {}, rho: {}", lambda, x, rho)
              << std::endl;
#endif
    return binarySearchCommon(reader, rho, lambda, x, idGenerator, tracker);
}

boost::optional<uint64_t> ESCCollection::binaryHops(HmacContext* context,
                                                    const FLEStateCollectionReader& reader,
                                                    const ESCTwiceDerivedTagToken& tagToken,
                                                    const ESCTwiceDerivedValueToken& valueToken,
                                                    boost::optional<uint64_t> x,
                                                    FLEStatusSection::EmuBinaryTracker& tracker) {
    uint64_t lambda;
    boost::optional<uint64_t> i;

    // 1. If no anchors present, then i = lambda = 0.
    //    Otherwise, get the anchor (either null or non-null),
    //    and set i = null and lambda = anchor.cpos
    if (x.has_value() && *x == 0) {
        i = 0;
        lambda = 0;
    } else {
        auto id = x.has_value() ? generateAnchorId(context, tagToken, *x)
                                : generateNullAnchorId(context, tagToken);
        auto doc = reader.getById(id);
        uassert(7291501, "ESC anchor document not found", !doc.isEmpty());

        auto swAnchor = decryptDocument(valueToken, doc);
        uassertStatusOK(swAnchor.getStatus());
        lambda = swAnchor.getValue().count;
        i = boost::none;
    }

    // 2-4. initialize rho based on ESC
    uint64_t rho = reader.getDocumentCount();
    if (rho < 2) {
        rho = 2;
    }

    auto idGenerator = [context, &tagToken](uint64_t value) -> PrfBlock {
        return generateNonAnchorId(context, tagToken, value);
    };

#ifdef DEBUG_ENUM_BINARY
    std::cout << fmt::format("binary search start: lambda: {}, i: {}, rho: {}", lambda, i, rho)
              << std::endl;
#endif
    return binarySearchCommon(reader, rho, lambda, i, idGenerator, tracker);
}

std::vector<std::vector<FLEEdgeCountInfo>> ESCCollection::getTags(
    const FLEStateCollectionReader& reader,
    const std::vector<std::vector<FLEEdgePrfBlock>>& tokensSets,
    FLETagQueryInterface::TagQueryType type) {

    HmacContext hmacCtx;
    std::vector<std::vector<FLEEdgeCountInfo>> countInfoSets;
    countInfoSets.reserve(tokensSets.size());

    for (const auto& tokens : tokensSets) {
        std::vector<FLEEdgeCountInfo> countInfos;
        countInfos.reserve(tokens.size());

        for (const auto& token : tokens) {
            switch (type) {
                case FLETagQueryInterface::TagQueryType::kCompact:
                    countInfos.push_back(getEdgeCountInfoForCompact(&hmacCtx, reader, token.esc));
                    break;
                case FLETagQueryInterface::TagQueryType::kCleanup:
                    countInfos.push_back(getEdgeCountInfoForCleanup(&hmacCtx, reader, token.esc));
                    break;
                case FLETagQueryInterface::TagQueryType::kPadding:
                    countInfos.push_back(getEdgeCountInfoForPadding(&hmacCtx, reader, token.esc));
                    break;
                case FLETagQueryInterface::TagQueryType::kInsert:
                case FLETagQueryInterface::TagQueryType::kQuery:
                    countInfos.push_back(
                        getEdgeCountInfo(&hmacCtx, reader, token.esc, type, token.edc));
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }

        countInfoSets.emplace_back(countInfos);
    }

    return countInfoSets;
}


FLE2FindEqualityPayloadV2 FLEClientCrypto::serializeFindPayloadV2(FLEIndexKeyAndId indexKey,
                                                                  FLEUserKeyAndId userKey,
                                                                  BSONElement element,
                                                                  uint64_t maxContentionFactor) {
    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = CollectionsLevel1Token::deriveFrom(indexKey.key);
    auto serverToken = ServerTokenDerivationLevel1Token::deriveFrom(indexKey.key);

    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);

    auto edcDatakey = EDCDerivedFromDataToken::deriveFrom(edcToken, value);
    auto escDatakey = ESCDerivedFromDataToken::deriveFrom(escToken, value);
    auto serverDataDerivedToken = ServerDerivedFromDataToken::deriveFrom(serverToken, value);

    FLE2FindEqualityPayloadV2 payload;

    payload.setEdcDerivedToken(std::move(edcDatakey));
    payload.setEscDerivedToken(std::move(escDatakey));
    payload.setMaxCounter(maxContentionFactor);
    payload.setServerDerivedFromDataToken(std::move(serverDataDerivedToken));

    return payload;
}


FLE2FindRangePayloadV2 FLEClientCrypto::serializeFindRangePayloadV2(
    FLEIndexKeyAndId indexKey,
    FLEUserKeyAndId userKey,
    const std::vector<std::string>& edges,
    uint64_t maxContentionFactor,
    uint32_t sparsity,
    const FLE2RangeFindSpec& spec) {
    auto collectionToken = CollectionsLevel1Token::deriveFrom(indexKey.key);
    auto serverToken = ServerTokenDerivationLevel1Token::deriveFrom(indexKey.key);

    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);

    std::vector<EdgeFindTokenSetV2> tokens;
    for (auto const& edge : edges) {

        ConstDataRange value(edge.c_str(), edge.size());

        EdgeFindTokenSetV2 tokenSet;
        tokenSet.setEdcDerivedToken(EDCDerivedFromDataToken::deriveFrom(edcToken, value));

        tokenSet.setEscDerivedToken(ESCDerivedFromDataToken::deriveFrom(escToken, value));
        tokenSet.setServerDerivedFromDataToken(
            ServerDerivedFromDataToken::deriveFrom(serverToken, value));
        tokens.push_back(std::move(tokenSet));
    }

    FLE2FindRangePayloadV2 payload;
    FLE2FindRangePayloadEdgesInfoV2 edgesInfo;

    edgesInfo.setEdges(std::move(tokens));
    edgesInfo.setMaxCounter(maxContentionFactor);

    payload.setPayload(edgesInfo);
    payload.setFirstOperator(spec.getFirstOperator());
    payload.setSecondOperator(spec.getSecondOperator());
    payload.setPayloadId(spec.getPayloadId());

    if (spec.getEdgesInfo().has_value()) {
        auto specEdgeInfo = spec.getEdgesInfo().get();
        payload.setPrecision(specEdgeInfo.getPrecision());
        payload.setTrimFactor(specEdgeInfo.getTrimFactor());
        payload.setIndexMin(specEdgeInfo.getIndexMin());
        payload.setIndexMax(specEdgeInfo.getIndexMax());
        payload.setSparsity(sparsity);
    }

    return payload;
}

FLE2FindRangePayloadV2 FLEClientCrypto::serializeFindRangeStubV2(const FLE2RangeFindSpec& spec) {
    FLE2FindRangePayloadV2 payload;

    payload.setFirstOperator(spec.getFirstOperator());
    payload.setSecondOperator(spec.getSecondOperator());
    payload.setPayloadId(spec.getPayloadId());

    return payload;
}

ECOCCompactionDocumentV2 ECOCCompactionDocumentV2::parseAndDecrypt(const BSONObj& doc,
                                                                   const ECOCToken& token) {
    IDLParserContext ctx("root");
    auto ecocDoc = EcocDocument::parse(doc, ctx);

    // The ecocDoc from EcocDocument::parse is const, so make a copy when decrypting.
    auto keys = StateCollectionTokensV2::Encrypted(ecocDoc.getValue()).decrypt(token);

    ECOCCompactionDocumentV2 ret;
    ret.fieldName = std::string{ecocDoc.getFieldName()};
    // Copy the ESC key over to prevent a segfault when the keys object gets deleted.
    ret.esc = ESCDerivedFromDataTokenAndContentionFactorToken(
        keys.getESCDerivedFromDataTokenAndContentionFactorToken());
    ret.isLeaf = keys.getIsLeaf();
    ret.msize = keys.getMsize();
    return ret;
}

FLE2TagAndEncryptedMetadataBlock::FLE2TagAndEncryptedMetadataBlock()
    : ConstFLE2TagAndEncryptedMetadataBlock(nullptr),
      _mblock(reinterpret_cast<_mc_FLE2TagAndEncryptedMetadataBlock_t*>(
                  bson_malloc0(sizeof(_mc_FLE2TagAndEncryptedMetadataBlock_t))),
              [](_mc_FLE2TagAndEncryptedMetadataBlock_t* blk) {
                  mc_FLE2TagAndEncryptedMetadataBlock_cleanup(blk);
                  bson_free(blk);
              }) {
    mc_FLE2TagAndEncryptedMetadataBlock_init(_mblock.get());
    _block = _mblock.get();
};

FLE2TagAndEncryptedMetadataBlock::FLE2TagAndEncryptedMetadataBlock(
    _mc_FLE2TagAndEncryptedMetadataBlock_t* mblock)
    : ConstFLE2TagAndEncryptedMetadataBlock(nullptr),
      _mblock(mblock, [](_mc_FLE2TagAndEncryptedMetadataBlock_t*) { /* unowned, don't free */ }) {
    // ensure the provided C struct is initialized
    mc_FLE2TagAndEncryptedMetadataBlock_init(_mblock.get());
    _block = _mblock.get();
}

Status FLE2TagAndEncryptedMetadataBlock::encryptAndSerialize(
    const ServerDerivedFromDataToken& token,
    uint64_t count,
    uint64_t contentionFactor,
    PrfBlock tag) {

    // clean up old data and reinitialize
    mc_FLE2TagAndEncryptedMetadataBlock_cleanup(_block);
    mc_FLE2TagAndEncryptedMetadataBlock_init(_block);

    auto countEncryptionToken = ServerCountAndContentionFactorEncryptionToken::deriveFrom(token);
    auto zerosEncryptionToken = ServerZerosEncryptionToken::deriveFrom(token);

    auto ecount =
        uassertStatusOK(packAndEncrypt(std::tie(count, contentionFactor), countEncryptionToken));
    dassert(ecount.size() == sizeof(EncryptedCountersBlob));

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &_block->encryptedCount, ecount.data(), ecount.size())) {
        return Status(ErrorCodes::LibmongocryptError,
                      "Unable to copy encrypted counts into buffer");
    }

    if (!_mongocrypt_buffer_copy_from_data_and_size(&_block->tag, tag.data(), sizeof(tag))) {
        mc_FLE2TagAndEncryptedMetadataBlock_cleanup(_block);
        return Status(ErrorCodes::LibmongocryptError, "Unable to copy PRF tag into buffer");
    }

    auto ezeros =
        uassertStatusOK(FLEUtil::encryptData(zerosEncryptionToken.toCDR(), ConstDataRange(kZeros)));
    dassert(ezeros.size() == sizeof(EncryptedZerosBlob));

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &_block->encryptedZeros, ezeros.data(), ezeros.size())) {
        mc_FLE2TagAndEncryptedMetadataBlock_cleanup(_block);
        return Status(ErrorCodes::LibmongocryptError, "Unable to copy encrypted zeros into buffer");
    }
    return Status::OK();
}

ConstFLE2TagAndEncryptedMetadataBlock::ConstFLE2TagAndEncryptedMetadataBlock(
    _mc_FLE2TagAndEncryptedMetadataBlock_t* mblock)
    : _block(mblock) {}

ConstFLE2TagAndEncryptedMetadataBlock::View ConstFLE2TagAndEncryptedMetadataBlock::getView() const {
    View result;
    result.encryptedCounts = MongoCryptBuffer::borrow(&_block->encryptedCount).toCDR();
    result.tag = MongoCryptBuffer::borrow(&_block->tag).toCDR();
    result.encryptedZeros = MongoCryptBuffer::borrow(&_block->encryptedZeros).toCDR();
    uassert(10164501,
            "Encountered Queryable Encryption encrypted counters with invalid size",
            result.encryptedCounts.length() ==
                sizeof(FLE2TagAndEncryptedMetadataBlock::EncryptedCountersBlob));
    uassert(10164502,
            "Encountered Queryable Encryption tag with invalid size",
            result.tag.length() == sizeof(PrfBlock));
    uassert(10164503,
            "Encountered Queryable Encryption encrypted zeros with invalid size",
            result.encryptedZeros.length() ==
                sizeof(FLE2TagAndEncryptedMetadataBlock::EncryptedZerosBlob));
    return result;
}

StatusWith<ConstFLE2TagAndEncryptedMetadataBlock::ZerosBlob>
ConstFLE2TagAndEncryptedMetadataBlock::decryptZerosBlob(
    ServerZerosEncryptionToken zerosEncryptionToken) const {
    auto block = getView();
    dassert(block.encryptedZeros.length() == sizeof(EncryptedZerosBlob));

    auto swDecryptedZeros =
        FLEUtil::decryptData(zerosEncryptionToken.toCDR(), block.encryptedZeros);
    if (!swDecryptedZeros.isOK()) {
        return swDecryptedZeros.getStatus();
    }
    ConstDataRangeCursor zerosCdrc(swDecryptedZeros.getValue());
    return zerosCdrc.readAndAdvanceNoThrow<ZerosBlob>();
}

StatusWith<ConstFLE2TagAndEncryptedMetadataBlock::CountersPair>
ConstFLE2TagAndEncryptedMetadataBlock::decryptCounterAndContentionFactorPair(
    ServerCountAndContentionFactorEncryptionToken countsEncryptionToken) const {
    auto block = getView();
    dassert(block.encryptedCounts.length() == sizeof(EncryptedCountersBlob));

    auto swCounters =
        decryptAndUnpack<uint64_t, uint64_t>(block.encryptedCounts, countsEncryptionToken);
    if (!swCounters.isOK()) {
        return swCounters.getStatus();
    }
    return CountersPair{.counter = std::get<0>(swCounters.getValue()),
                        .contentionFactor = std::get<1>(swCounters.getValue())};
}

bool ConstFLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(const ZerosBlob& blob) {
    ConstDataRangeCursor cdrc(blob);
    uint64_t high = cdrc.readAndAdvance<uint64_t>();
    uint64_t low = cdrc.readAndAdvance<uint64_t>();
    return !(high | low);
}

FLE2IndexedEqualityEncryptedValueV2::FLE2IndexedEqualityEncryptedValueV2()
    : _value(mc_FLE2IndexedEncryptedValueV2_new()) {}

FLE2IndexedEqualityEncryptedValueV2::FLE2IndexedEqualityEncryptedValueV2(ConstDataRange cdr)
    : _value(mc_FLE2IndexedEncryptedValueV2_new()) {
    auto buf = MongoCryptBuffer::borrow(cdr);
    MongoCryptStatus status;
    mc_FLE2IndexedEncryptedValueV2_parse(_value.get(), buf.get(), status);
    uassertStatusOK(status.toStatus());
    uassert(9588708,
            fmt::format("Expected buffer to begin with type tag {}, but began with {}",
                        fmt::underlying(kFLE2IEVTypeEqualityV2),
                        fmt::underlying(_value->type)),
            _value->type == kFLE2IEVTypeEqualityV2);
}

FLE2IndexedEqualityEncryptedValueV2 FLE2IndexedEqualityEncryptedValueV2::fromUnencrypted(
    const FLE2InsertUpdatePayloadV2& payload, PrfBlock tag, uint64_t counter) {

    uassert(9697301,
            "Invalid BSON Type in Queryable Encryption InsertUpdatePayloadV2",
            isValidBSONType(payload.getType()));

    BSONType bsonType = static_cast<BSONType>(payload.getType());
    uassert(7291906,
            str::stream() << "Type '" << typeName(bsonType)
                          << "' is not a valid type for Queryable Encryption Equality",
            isFLE2EqualityIndexedSupportedType(bsonType));

    auto clientEncryptedValue(FLEUtil::vectorFromCDR(payload.getValue()));
    uassert(9697302,
            "Invalid client encrypted value length for FLE2IndexedEqualityEncryptedValueV2",
            !clientEncryptedValue.empty());

    FLE2IndexedEqualityEncryptedValueV2 value;
    mc_FLE2IndexedEncryptedValueV2_t* iev = value._value.get();

    iev->type = kFLE2IEVTypeEqualityV2;
    iev->edge_count = 1;
    iev->fle_blob_subtype = static_cast<int8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValueV2);
    iev->bson_value_type = stdx::to_underlying(bsonType);

    auto keyId = payload.getIndexKeyId().toCDR();

    auto serverEncryptedValue = uassertStatusOK(FLEUtil::encryptData(
        payload.getServerEncryptionToken().toCDR(), ConstDataRange(clientEncryptedValue)));

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &iev->S_KeyId, reinterpret_cast<const uint8_t*>(keyId.data()), keyId.length())) {
        uassertStatusOK(
            Status(ErrorCodes::LibmongocryptError, "Unable to copy S_KeyId into buffer"));
    }

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &iev->ServerEncryptedValue, serverEncryptedValue.data(), serverEncryptedValue.size())) {
        uassertStatusOK(Status(ErrorCodes::LibmongocryptError,
                               "Unable to copy ServerEncryptedValue into buffer"));
    }

    iev->metadata = reinterpret_cast<mc_FLE2TagAndEncryptedMetadataBlock_t*>(
        bson_malloc(sizeof(mc_FLE2TagAndEncryptedMetadataBlock_t)));

    uassertStatusOK(FLE2TagAndEncryptedMetadataBlock(iev->metadata)
                        .encryptAndSerialize(payload.getServerDerivedFromDataToken(),
                                             counter,
                                             payload.getContentionFactor(),
                                             tag));
    MongoCryptStatus status;
    if (!mc_FLE2IndexedEncryptedValueV2_validate(iev, status)) {
        uassertStatusOK(status.toStatus());
    };
    return value;
}

ConstDataRange FLE2IndexedEqualityEncryptedValueV2::getServerEncryptedValue() const {
    if (!_cachedServerEncryptedValue) {
        _cachedServerEncryptedValue =
            MongoCryptBuffer::borrow(&_value->ServerEncryptedValue).toCDR();
    }
    return *_cachedServerEncryptedValue;
}

PrfBlock FLE2IndexedEqualityEncryptedValueV2::getMetadataBlockTag() const {
    if (!_cachedMetadataBlockTag) {
        _cachedMetadataBlockTag =
            PrfBlockfromCDR(MongoCryptBuffer::borrow(&_value->metadata->tag).toCDR());
    }
    return *_cachedMetadataBlockTag;
}

ConstFLE2TagAndEncryptedMetadataBlock FLE2IndexedEqualityEncryptedValueV2::getRawMetadataBlock()
    const {
    return ConstFLE2TagAndEncryptedMetadataBlock(_value->metadata);
}

UUID FLE2IndexedEqualityEncryptedValueV2::getKeyId() const {
    if (!_cachedKeyId) {
        _cachedKeyId = UUID::fromCDR(MongoCryptBuffer::borrow(&_value->S_KeyId).toCDR());
    }
    return *_cachedKeyId;
}

BSONType FLE2IndexedEqualityEncryptedValueV2::getBsonType() const {
    return BSONType(_value->bson_value_type);
}

StatusWith<std::vector<uint8_t>> FLE2IndexedEqualityEncryptedValueV2::serialize() const {
    if (!_cachedSerializedPayload) {
        MongoCryptStatus status;
        MongoCryptBuffer buf;
        if (!mc_FLE2IndexedEncryptedValueV2_serialize(_value.get(), buf.get(), status)) {
            return status.toStatus();
        }
        _cachedSerializedPayload = std::vector<uint8_t>(buf.data(), buf.data() + buf.size());
    }
    return *_cachedSerializedPayload;
}

FLE2IndexedRangeEncryptedValueV2::FLE2IndexedRangeEncryptedValueV2()
    : _value(mc_FLE2IndexedEncryptedValueV2_new()) {}

FLE2IndexedRangeEncryptedValueV2::FLE2IndexedRangeEncryptedValueV2(ConstDataRange toParse)
    : _value(mc_FLE2IndexedEncryptedValueV2_new()) {
    auto buf = MongoCryptBuffer::borrow(toParse);
    MongoCryptStatus status;
    mc_FLE2IndexedEncryptedValueV2_parse(_value.get(), buf.get(), status);
    uassertStatusOK(status.toStatus());
    uassert(9588706,
            fmt::format("Expected buffer to begin with type tag {}, but began with {}",
                        fmt::underlying(kFLE2IEVTypeRangeV2),
                        fmt::underlying(_value->type)),
            _value->type == kFLE2IEVTypeRangeV2);
}

FLE2IndexedRangeEncryptedValueV2 FLE2IndexedRangeEncryptedValueV2::fromUnencrypted(
    const FLE2InsertUpdatePayloadV2& payload,
    const std::vector<PrfBlock>& tags,
    const std::vector<uint64_t>& counters) {

    // Range-indexed fields can only have at most 129 tags (128 edges for decimal128 + 1 root)
    // per OST.
    static constexpr size_t kFLE2RangeFieldMaxTags = 129;

    uassert(9588700,
            "Non-range search InsertUpdatePayload supplied for FLE2IndexedRangeEncryptedValueV2",
            payload.getEdgeTokenSet().has_value());
    uassert(9588701,
            "Invalid BSON Type in Queryable Encryption InsertUpdatePayloadV2",
            isValidBSONType(payload.getType()));

    BSONType bsonType = static_cast<BSONType>(payload.getType());
    uassert(7291908,
            str::stream() << "Type '" << typeName(bsonType)
                          << "' is not a valid type for Queryable Encryption Range",
            isFLE2RangeIndexedSupportedType(bsonType));

    auto& ets = payload.getEdgeTokenSet().value();

    // Ensure the total tags will not overflow the per-field tag limit.
    uassert(9588702,
            "InsertUpdatePayload for range-indexed field has an edge token set that is too large",
            ets.size() <= kFLE2RangeFieldMaxTags);
    uassert(9588703,
            "FLE2IndexedRangeEncryptedValueV2 tags length must equal the total number of edges",
            tags.size() == ets.size());
    uassert(9588704,
            "FLE2IndexedRangeEncryptedValueV2 counters length must equal the total number of edges",
            counters.size() == ets.size());

    auto clientEncryptedValue(FLEUtil::vectorFromCDR(payload.getValue()));
    uassert(9588705,
            "Invalid client encrypted value length for FLE2IndexedRangeEncryptedValueV2",
            !clientEncryptedValue.empty());

    FLE2IndexedRangeEncryptedValueV2 value;
    mc_FLE2IndexedEncryptedValueV2_t* iev = value._value.get();

    iev->type = kFLE2IEVTypeRangeV2;
    iev->fle_blob_subtype = static_cast<int8_t>(EncryptedBinDataType::kFLE2RangeIndexedValueV2);
    iev->bson_value_type = stdx::to_underlying(bsonType);
    iev->edge_count = static_cast<uint32_t>(ets.size());

    auto keyId = payload.getIndexKeyId().toCDR();

    auto serverEncryptedValue = uassertStatusOK(FLEUtil::encryptData(
        payload.getServerEncryptionToken().toCDR(), ConstDataRange(clientEncryptedValue)));

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &iev->S_KeyId, reinterpret_cast<const uint8_t*>(keyId.data()), keyId.length())) {
        uassertStatusOK(
            Status(ErrorCodes::LibmongocryptError, "Unable to copy S_KeyId into buffer"));
    }

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &iev->ServerEncryptedValue, serverEncryptedValue.data(), serverEncryptedValue.size())) {
        uassertStatusOK(Status(ErrorCodes::LibmongocryptError,
                               "Unable to copy ServerEncryptedValue into buffer"));
    }

    // Create a metadata block for each edge
    iev->metadata = reinterpret_cast<mc_FLE2TagAndEncryptedMetadataBlock_t*>(
        bson_malloc(ets.size() * sizeof(mc_FLE2TagAndEncryptedMetadataBlock_t)));

    MongoCryptStatus status;
    for (size_t i = 0; i < ets.size(); i++) {
        auto& serverDataDerivedToken = ets[i].getServerDerivedFromDataToken();
        uassertStatusOK(
            FLE2TagAndEncryptedMetadataBlock(&iev->metadata[i])
                .encryptAndSerialize(
                    serverDataDerivedToken, counters[i], payload.getContentionFactor(), tags[i]));
    }

    if (!mc_FLE2IndexedEncryptedValueV2_validate(iev, status)) {
        uassertStatusOK(status.toStatus());
    };
    return value;
}

StatusWith<std::vector<uint8_t>> FLE2IndexedRangeEncryptedValueV2::serialize() const {
    if (!_cachedSerializedPayload) {
        MongoCryptStatus status;
        MongoCryptBuffer buf;
        if (!mc_FLE2IndexedEncryptedValueV2_serialize(_value.get(), buf.get(), status)) {
            return status.toStatus();
        }
        _cachedSerializedPayload = std::vector<uint8_t>(buf.data(), buf.data() + buf.size());
    }

    return *_cachedSerializedPayload;
}

UUID FLE2IndexedRangeEncryptedValueV2::getKeyId() const {
    return UUID::fromCDR(MongoCryptBuffer::borrow(&_value->S_KeyId).toCDR());
}

BSONType FLE2IndexedRangeEncryptedValueV2::getBsonType() const {
    return BSONType(_value->bson_value_type);
}

uint32_t FLE2IndexedRangeEncryptedValueV2::getTagCount() const {
    return _value->edge_count;
}

ConstDataRange FLE2IndexedRangeEncryptedValueV2::getServerEncryptedValue() const {
    return MongoCryptBuffer::borrow(&_value->ServerEncryptedValue).toCDR();
}

std::vector<ConstFLE2TagAndEncryptedMetadataBlock>
FLE2IndexedRangeEncryptedValueV2::getMetadataBlocks() const {
    std::vector<ConstFLE2TagAndEncryptedMetadataBlock> res;
    for (size_t i = 0; i < getTagCount(); i++) {
        res.emplace_back(&_value->metadata[i]);
    }
    return res;
}

FLE2IndexedTextEncryptedValue::FLE2IndexedTextEncryptedValue()
    : _value(mc_FLE2IndexedEncryptedValueV2_new()) {}

FLE2IndexedTextEncryptedValue::FLE2IndexedTextEncryptedValue(ConstDataRange toParse)
    : _value(mc_FLE2IndexedEncryptedValueV2_new()) {
    auto buf = MongoCryptBuffer::borrow(toParse);
    MongoCryptStatus status;
    mc_FLE2IndexedEncryptedValueV2_parse(_value.get(), buf.get(), status);
    uassertStatusOK(status.toStatus());
    uassert(9784115,
            fmt::format("Expected buffer to begin with type tag {}, but began with {}",
                        fmt::underlying(kFLE2IEVTypeText),
                        fmt::underlying(_value->type)),
            _value->type == kFLE2IEVTypeText);
}

FLE2IndexedTextEncryptedValue FLE2IndexedTextEncryptedValue::fromUnencrypted(
    const FLE2InsertUpdatePayloadV2& payload,
    const std::vector<PrfBlock>& tags,
    const std::vector<uint64_t>& counters) {

    uassert(9784102,
            "Non-text search InsertUpdatePayload supplied for FLE2IndexedTextEncryptedValue",
            payload.getTextSearchTokenSets().has_value());
    uassert(9784103,
            "InsertUpdatePayload has bad BSON type for FLE2IndexedTextEncryptedValue",
            static_cast<BSONType>(payload.getType()) == BSONType::string);

    auto& tsts = payload.getTextSearchTokenSets().value();

    // Ensure the total tags will not overflow the per-field tag limit.
    uint32_t tagLimit = EncryptionInformationHelpers::kFLE2PerFieldTagLimit -
        1;  // subtract one for exact match tag

    uassert(9784104,
            "InsertUpdatePayload substring token sets size is too large",
            tsts.getSubstringTokenSets().size() <= tagLimit);
    uint32_t substrTagCount = static_cast<uint32_t>(tsts.getSubstringTokenSets().size());
    tagLimit -= substrTagCount;

    uassert(9784105,
            "InsertUpdatePayload suffix token sets size is too large",
            tsts.getSuffixTokenSets().size() <= tagLimit);
    uint32_t suffixTagCount = static_cast<uint32_t>(tsts.getSuffixTokenSets().size());
    tagLimit -= suffixTagCount;

    uassert(9784106,
            "InsertUpdatePayload prefix token sets size is too large",
            tsts.getPrefixTokenSets().size() <= tagLimit);
    uint32_t totalTagCount = 1 + substrTagCount + suffixTagCount +
        static_cast<uint32_t>(tsts.getPrefixTokenSets().size());

    uassert(9784113,
            "FLE2IndexedTextEncryptedValue tags length must equal the total number of text "
            "search token sets",
            tags.size() == totalTagCount);
    uassert(9784107,
            "FLE2IndexedTextEncryptedValue counters length must equal the total number of text "
            "search token sets",
            counters.size() == totalTagCount);
    auto clientEncryptedValue(FLEUtil::vectorFromCDR(payload.getValue()));
    uassert(9784108,
            "Invalid client encrypted value length for FLE2IndexedTextEncryptedValue",
            !clientEncryptedValue.empty());

    FLE2IndexedTextEncryptedValue value;
    mc_FLE2IndexedEncryptedValueV2_t* iev = value._value.get();

    iev->type = kFLE2IEVTypeText;
    iev->fle_blob_subtype = static_cast<int8_t>(EncryptedBinDataType::kFLE2TextIndexedValue);
    iev->bson_value_type = stdx::to_underlying(static_cast<BSONType>(payload.getType()));
    iev->edge_count = totalTagCount;
    iev->substr_tag_count = substrTagCount;
    iev->suffix_tag_count = suffixTagCount;

    auto keyId = payload.getIndexKeyId().toCDR();

    auto serverEncryptedValue = uassertStatusOK(FLEUtil::encryptData(
        payload.getServerEncryptionToken().toCDR(), ConstDataRange(clientEncryptedValue)));

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &iev->S_KeyId, reinterpret_cast<const uint8_t*>(keyId.data()), keyId.length())) {
        uassertStatusOK(
            Status(ErrorCodes::LibmongocryptError, "Unable to copy S_KeyId into buffer"));
    }

    if (!_mongocrypt_buffer_copy_from_data_and_size(
            &iev->ServerEncryptedValue, serverEncryptedValue.data(), serverEncryptedValue.size())) {
        uassertStatusOK(Status(ErrorCodes::LibmongocryptError,
                               "Unable to copy ServerEncryptedValue into buffer"));
    }

    // Collect serverDerivedFromDataTokens from all text search token sets, flattened into a list.
    // The order of addition to the list is important!
    std::vector<PrfBlock> serverDerivedFromDataTokens;
    serverDerivedFromDataTokens.reserve(totalTagCount);

    serverDerivedFromDataTokens.push_back(
        tsts.getExactTokenSet().getServerDerivedFromDataToken().asPrfBlock());
    for (const auto& ts : tsts.getSubstringTokenSets()) {
        serverDerivedFromDataTokens.push_back(ts.getServerDerivedFromDataToken().asPrfBlock());
    }
    for (const auto& ts : tsts.getSuffixTokenSets()) {
        serverDerivedFromDataTokens.push_back(ts.getServerDerivedFromDataToken().asPrfBlock());
    }
    for (const auto& ts : tsts.getPrefixTokenSets()) {
        serverDerivedFromDataTokens.push_back(ts.getServerDerivedFromDataToken().asPrfBlock());
    }
    dassert(totalTagCount == serverDerivedFromDataTokens.size());

    iev->metadata = reinterpret_cast<mc_FLE2TagAndEncryptedMetadataBlock_t*>(
        bson_malloc(totalTagCount * sizeof(mc_FLE2TagAndEncryptedMetadataBlock_t)));

    MongoCryptStatus status;
    for (size_t i = 0; i < totalTagCount; i++) {
        auto serverDataDerivedToken = ServerDerivedFromDataToken(serverDerivedFromDataTokens[i]);
        uassertStatusOK(
            FLE2TagAndEncryptedMetadataBlock(&iev->metadata[i])
                .encryptAndSerialize(
                    serverDataDerivedToken, counters[i], payload.getContentionFactor(), tags[i]));
    }

    if (!mc_FLE2IndexedEncryptedValueV2_validate(iev, status)) {
        uassertStatusOK(status.toStatus());
    };
    return value;
}

StatusWith<std::vector<uint8_t>> FLE2IndexedTextEncryptedValue::serialize() const {
    if (!_cachedSerializedPayload) {
        MongoCryptStatus status;
        MongoCryptBuffer buf;
        if (!mc_FLE2IndexedEncryptedValueV2_serialize(_value.get(), buf.get(), status)) {
            return status.toStatus();
        }
        _cachedSerializedPayload = std::vector<uint8_t>(buf.data(), buf.data() + buf.size());
    }

    return *_cachedSerializedPayload;
}

UUID FLE2IndexedTextEncryptedValue::getKeyId() const {
    return UUID::fromCDR(MongoCryptBuffer::borrow(&_value->S_KeyId).toCDR());
}

BSONType FLE2IndexedTextEncryptedValue::getBsonType() const {
    return BSONType(_value->bson_value_type);
}

ConstDataRange FLE2IndexedTextEncryptedValue::getServerEncryptedValue() const {
    return MongoCryptBuffer::borrow(&_value->ServerEncryptedValue).toCDR();
}

uint32_t FLE2IndexedTextEncryptedValue::getTagCount() const {
    return _value->edge_count;
}

uint32_t FLE2IndexedTextEncryptedValue::getSubstringTagCount() const {
    return _value->substr_tag_count;
}

uint32_t FLE2IndexedTextEncryptedValue::getSuffixTagCount() const {
    return _value->suffix_tag_count;
}

uint32_t FLE2IndexedTextEncryptedValue::getPrefixTagCount() const {
    auto otherTagCount = getSubstringTagCount() + getSuffixTagCount() + 1;
    dassert(getTagCount() >= otherTagCount);
    return getTagCount() - otherTagCount;
}

ConstFLE2TagAndEncryptedMetadataBlock FLE2IndexedTextEncryptedValue::getExactStringMetadataBlock()
    const {
    return ConstFLE2TagAndEncryptedMetadataBlock(&_value->metadata[0]);
}

std::vector<ConstFLE2TagAndEncryptedMetadataBlock>
FLE2IndexedTextEncryptedValue::getSubstringMetadataBlocks() const {
    std::vector<ConstFLE2TagAndEncryptedMetadataBlock> res;
    constexpr size_t offset = 1;
    for (size_t i = offset; i < offset + getSubstringTagCount(); i++) {
        res.emplace_back(&_value->metadata[i]);
    }
    return res;
}

std::vector<ConstFLE2TagAndEncryptedMetadataBlock>
FLE2IndexedTextEncryptedValue::getSuffixMetadataBlocks() const {
    std::vector<ConstFLE2TagAndEncryptedMetadataBlock> res;
    const size_t offset = 1 + getSubstringTagCount();
    for (size_t i = offset; i < offset + getSuffixTagCount(); i++) {
        res.emplace_back(&_value->metadata[i]);
    }
    return res;
}

std::vector<ConstFLE2TagAndEncryptedMetadataBlock>
FLE2IndexedTextEncryptedValue::getPrefixMetadataBlocks() const {
    std::vector<ConstFLE2TagAndEncryptedMetadataBlock> res;
    const size_t offset = 1 + getSubstringTagCount() + getSuffixTagCount();
    for (size_t i = offset; i < offset + getPrefixTagCount(); i++) {
        res.emplace_back(&_value->metadata[i]);
    }
    return res;
}

std::vector<ConstFLE2TagAndEncryptedMetadataBlock>
FLE2IndexedTextEncryptedValue::getAllMetadataBlocks() const {
    std::vector<ConstFLE2TagAndEncryptedMetadataBlock> res;
    for (size_t i = 0; i < getTagCount(); i++) {
        res.emplace_back(&_value->metadata[i]);
    }
    return res;
}

ESCDerivedFromDataTokenAndContentionFactorToken EDCServerPayloadInfo::getESCToken(
    ConstDataRange cdr) {
    return ESCDerivedFromDataTokenAndContentionFactorToken::parse(cdr);
}

void EDCServerCollection::validateEncryptedFieldInfo(BSONObj& obj,
                                                     const EncryptedFieldConfig& efc,
                                                     bool bypassDocumentValidation) {
    stdx::unordered_set<std::string> indexedFields;
    for (const auto& f : efc.getFields()) {
        if (f.getQueries().has_value()) {
            indexedFields.insert(std::string{f.getPath()});
        }
    }

    visitEncryptedBSON(obj, [&indexedFields](ConstDataRange cdr, StringData fieldPath) {
        auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);

        if (encryptedTypeBinding == EncryptedBinDataType::kFLE2InsertUpdatePayloadV2) {
            uassert(6373601,
                    str::stream() << "Field '" << fieldPath
                                  << "' is encrypted, but absent from schema",
                    indexedFields.contains(std::string{fieldPath}));
        }
    });

    // We should ensure that the user is not manually modifying the safe content array.
    uassert(6666200,
            str::stream() << "Cannot modify " << kSafeContent << " field in document.",
            !obj.hasField(kSafeContent) || bypassDocumentValidation);
}

void EDCServerCollection::validateModifiedDocumentCompatibility(BSONObj& obj) {
    visitEncryptedBSON(obj, [](ConstDataRange cdr, StringData fieldPath) {
        auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
        switch (encryptedTypeBinding) {
            case EncryptedBinDataType::kFLE2EqualityIndexedValue:
            case EncryptedBinDataType::kFLE2RangeIndexedValue:
            case EncryptedBinDataType::kFLE2UnindexedEncryptedValue:
                uasserted(7293202,
                          str::stream()
                              << "Cannot modify field '" << fieldPath
                              << "' because the encrypted value version is not compatible with the "
                                 "current Queryable Encryption protocol version");
            default:
                break;
        };
    });
}

std::vector<EDCServerPayloadInfo> EDCServerCollection::getEncryptedFieldInfo(BSONObj& obj) {
    std::vector<EDCServerPayloadInfo> fields;
    visitEncryptedBSON(obj, [&fields](ConstDataRange cdr, StringData fieldPath) {
        collectEDCServerInfo(&fields, cdr, fieldPath);
    });

    // Create collection checks for unique index key ids but users can supply schema client-side
    // We check here at runtime that all fields index keys are unique.
    stdx::unordered_set<UUID, UUID::Hash> indexKeyIds;
    for (const auto& field : fields) {
        auto& indexKeyId = field.payload.getIndexKeyId();
        uassert(6371407,
                "Index key ids must be unique across fields in a document",
                !indexKeyIds.contains(indexKeyId));
        indexKeyIds.insert(indexKeyId);
    }

    return fields;
}

PrfBlock EDCServerCollection::generateTag(HmacContext* hmacCtx,
                                          EDCTwiceDerivedToken edcTwiceDerived,
                                          FLECounter count) {
    return FLEUtil::prf(hmacCtx, edcTwiceDerived.toCDR(), count);
}

PrfBlock EDCServerCollection::generateTag(const EDCServerPayloadInfo& payload) {
    auto edcTwiceDerived = EDCTwiceDerivedToken::deriveFrom(payload.payload.getEdcDerivedToken());
    dassert(payload.isRangePayload() == false);
    dassert(payload.counts.size() == 1);
    HmacContext obj;
    return generateTag(&obj, edcTwiceDerived, payload.counts[0]);
}

std::vector<PrfBlock> EDCServerCollection::generateTagsForRange(
    const EDCServerPayloadInfo& rangePayload) {
    // throws if EDCServerPayloadInfo has invalid payload version
    auto& v2Payload = rangePayload.payload;

    uassert(7291909,
            "InsertUpdatePayload must have an edge token set",
            v2Payload.getEdgeTokenSet().has_value());
    uassert(7291910,
            "Mismatch between edge token set and counters lengths",
            v2Payload.getEdgeTokenSet()->size() == rangePayload.counts.size());

    auto& edgeTokenSets = v2Payload.getEdgeTokenSet().value();
    std::vector<PrfBlock> tags;
    tags.reserve(edgeTokenSets.size());

    HmacContext obj;
    for (size_t i = 0; i < edgeTokenSets.size(); i++) {
        auto edcTwiceDerived =
            EDCTwiceDerivedToken::deriveFrom(edgeTokenSets[i].getEdcDerivedToken());
        tags.push_back(
            EDCServerCollection::generateTag(&obj, edcTwiceDerived, rangePayload.counts[i]));
    }
    return tags;
}

std::vector<PrfBlock> EDCServerCollection::generateTagsForTextSearch(
    const EDCServerPayloadInfo& textPayload) {
    auto totalTagCount = textPayload.getTotalTextSearchTokenSetCount();

    uassert(9784110,
            "InsertUpdatePayload must have a text search token set",
            textPayload.isTextSearchPayload());
    uassert(9784111,
            "Mismatch between total text search token set count and counters lengths",
            totalTagCount == textPayload.counts.size());

    auto& tsts = textPayload.payload.getTextSearchTokenSets().value();

    // Collect edcDerivedTokens from all text search token sets, flattened into a list
    std::vector<PrfBlock> edcDerivedTokens;
    edcDerivedTokens.reserve(totalTagCount);

    edcDerivedTokens.push_back(tsts.getExactTokenSet().getEdcDerivedToken().asPrfBlock());
    for (const auto& ts : tsts.getSubstringTokenSets()) {
        edcDerivedTokens.push_back(ts.getEdcDerivedToken().asPrfBlock());
    }
    for (const auto& ts : tsts.getSuffixTokenSets()) {
        edcDerivedTokens.push_back(ts.getEdcDerivedToken().asPrfBlock());
    }
    for (const auto& ts : tsts.getPrefixTokenSets()) {
        edcDerivedTokens.push_back(ts.getEdcDerivedToken().asPrfBlock());
    }
    dassert(totalTagCount == edcDerivedTokens.size());

    std::vector<PrfBlock> tags;
    tags.reserve(totalTagCount);

    HmacContext hmacCtx;
    for (size_t i = 0; i < totalTagCount; i++) {
        auto edcTwiceDerived = EDCTwiceDerivedToken::deriveFrom(
            EDCDerivedFromDataTokenAndContentionFactor(edcDerivedTokens[i]));
        tags.push_back(
            EDCServerCollection::generateTag(&hmacCtx, edcTwiceDerived, textPayload.counts[i]));
    }
    return tags;
}

std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> EDCServerCollection::generateEDCTokens(
    EDCDerivedFromDataToken token, uint64_t maxContentionFactor) {
    std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> tokens;
    tokens.reserve(maxContentionFactor);

    for (uint64_t i = 0; i <= maxContentionFactor; ++i) {
        tokens.push_back(EDCDerivedFromDataTokenAndContentionFactorToken::deriveFrom(token, i));
    }

    return tokens;
}

std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> EDCServerCollection::generateEDCTokens(
    ConstDataRange rawToken, uint64_t maxContentionFactor) {
    auto token = EDCDerivedFromDataToken::parse(rawToken);

    return generateEDCTokens(token, maxContentionFactor);
}

BSONObj EDCServerCollection::finalizeForInsert(
    const BSONObj& doc, const std::vector<EDCServerPayloadInfo>& serverPayload) {
    std::vector<TagInfo> tags;
    tags.reserve(getEstimatedTagCount(serverPayload));

    ConstVectorIteratorPair<EDCServerPayloadInfo> it(serverPayload);

    // First: transform all the markings
    auto obj = transformBSON(
        doc, [&tags, &it](ConstDataRange cdr, BSONObjBuilder* builder, StringData fieldPath) {
            convertServerPayload(cdr, &tags, it, builder, fieldPath);
        });

    BSONObjBuilder builder;

    // Second: reuse an existing array if present
    bool appendElements = true;
    for (const auto& element : obj) {
        if (element.fieldNameStringData() == kSafeContent) {
            uassert(6373510,
                    str::stream() << "Field '" << kSafeContent << "' was found but not an array",
                    element.type() == BSONType::array);
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

BSONObj EDCServerCollection::finalizeForUpdate(
    const BSONObj& doc, const std::vector<EDCServerPayloadInfo>& serverPayload) {
    std::vector<TagInfo> tags;
    tags.reserve(getEstimatedTagCount(serverPayload));

    ConstVectorIteratorPair<EDCServerPayloadInfo> it(serverPayload);

    // First: transform all the markings
    auto obj = transformBSON(
        doc, [&tags, &it](ConstDataRange cdr, BSONObjBuilder* builder, StringData fieldPath) {
            convertServerPayload(cdr, &tags, it, builder, fieldPath);
        });

    BSONObjBuilder builder;

    // Second: reuse an existing array if present if we have tags to append.
    bool appendElements = true;
    for (const auto& element : obj) {
        // Only need to process $push if we have tags to append
        if (tags.size() > 0 && element.fieldNameStringData() == kDollarPush) {
            uassert(6371511,
                    str::stream() << "Field '" << kDollarPush << "' was found but not an object",
                    element.type() == BSONType::object);
            BSONObjBuilder subBuilder(builder.subobjStart(kDollarPush));

            // Append existing fields elements
            for (const auto& subElement : element.Obj()) {
                // Since we cannot be sure if the $push the server wants to perform (i.e. a $push
                // with $each) we can stop the client from sending $push. They can always submit it
                // via an unencrypted client.
                uassert(6371512,
                        str::stream() << "Cannot $push to " << kSafeContent,
                        subElement.fieldNameStringData() != kSafeContent);
                subBuilder.append(subElement);
            }

            // Build {$push: { _safeContent__ : {$each: [tag...]} }
            BSONObjBuilder pushBuilder(subBuilder.subobjStart(kSafeContent));
            {
                BSONArrayBuilder arrayBuilder(pushBuilder.subarrayStart(kDollarEach));

                // Add new tags
                for (auto const& tag : tags) {
                    appendTag(tag.tag, &arrayBuilder);
                }
            }

            appendElements = false;
        } else {
            builder.append(element);
        }
    }

    // Third: append the tags array if it does not exist
    if (appendElements && tags.size() > 0) {
        // Build {$push: { _safeContent__ : {$each: [tag...]} }
        BSONObjBuilder subBuilder(builder.subobjStart(kDollarPush));
        BSONObjBuilder pushBuilder(subBuilder.subobjStart(kSafeContent));
        {
            BSONArrayBuilder arrayBuilder(pushBuilder.subarrayStart(kDollarEach));

            // Add new tags
            for (auto const& tag : tags) {
                appendTag(tag.tag, &arrayBuilder);
            }
        }
    }

    return builder.obj();
}

BSONObj EDCServerCollection::generateUpdateToRemoveTags(const std::vector<PrfBlock>& tagsToPull) {
    uassert(7293203,
            "Cannot generate update command to remove tags with empty tags",
            !tagsToPull.empty());

    // Build { $pull : {__safeContent__ : {$in : [tag..] } } }
    BSONObjBuilder builder;
    {
        BSONObjBuilder subBuilder(builder.subobjStart(kDollarPull));
        BSONObjBuilder pushBuilder(subBuilder.subobjStart(kSafeContent));
        BSONArrayBuilder arrayBuilder(pushBuilder.subarrayStart(kDollarIn));

        // Add new tags
        for (const auto& tag : tagsToPull) {
            appendTag(tag, &arrayBuilder);
        }
    }
    return builder.obj();
}

std::vector<EDCIndexedFields> EDCServerCollection::getRemovedFields(
    std::vector<EDCIndexedFields>& originalDocument, std::vector<EDCIndexedFields>& newDocument) {
    std::sort(originalDocument.begin(), originalDocument.end());
    std::sort(newDocument.begin(), newDocument.end());

    std::vector<EDCIndexedFields> removedTags;
    std::set_difference(originalDocument.begin(),
                        originalDocument.end(),
                        newDocument.begin(),
                        newDocument.end(),
                        std::back_inserter(removedTags));

    return removedTags;
}

std::vector<PrfBlock> EDCServerCollection::getRemovedTags(
    std::vector<EDCIndexedFields>& originalDocument, std::vector<EDCIndexedFields>& newDocument) {
    auto deletedFields = EDCServerCollection::getRemovedFields(originalDocument, newDocument);
    std::vector<PrfBlock> staleTags;

    // Lower bound tag count is the number of removed fields.
    staleTags.reserve(deletedFields.size());

    for (auto& field : deletedFields) {
        auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(field.value);

        if (encryptedTypeBinding == EncryptedBinDataType::kFLE2EqualityIndexedValueV2) {
            auto tag = FLE2IndexedEqualityEncryptedValueV2(field.value).getMetadataBlockTag();
            staleTags.push_back(tag);
        } else if (encryptedTypeBinding == EncryptedBinDataType::kFLE2RangeIndexedValueV2) {
            FLE2IndexedRangeEncryptedValueV2 iev(field.value);
            auto metadata = iev.getMetadataBlocks();
            std::transform(metadata.begin(),
                           metadata.end(),
                           std::back_inserter(staleTags),
                           [](const auto& block) { return PrfBlockfromCDR(block.getView().tag); });
        } else if (encryptedTypeBinding == EncryptedBinDataType::kFLE2TextIndexedValue) {
            FLE2IndexedTextEncryptedValue iev(field.value);
            auto textMetadata = iev.getAllMetadataBlocks();
            std::transform(textMetadata.begin(),
                           textMetadata.end(),
                           std::back_inserter(staleTags),
                           [](const auto& block) { return PrfBlockfromCDR(block.getView().tag); });
        } else {
            auto typeValue = EncryptedBinDataType_serializer(encryptedTypeBinding);
            uasserted(7293204,
                      str::stream() << "Field '" << field.fieldPathName
                                    << "' is not a supported encrypted type: " << typeValue);
        }
    }
    return staleTags;
}

std::vector<EDCIndexedFields> EDCServerCollection::getEncryptedIndexedFields(BSONObj& obj) {
    std::vector<EDCIndexedFields> fields;

    visitEncryptedBSON(obj, [&fields](ConstDataRange cdr, StringData fieldPath) {
        collectIndexedFields(&fields, cdr, fieldPath);
    });

    return fields;
}

BSONObj EncryptionInformationHelpers::encryptionInformationSerialize(
    const NamespaceString& nss, const EncryptedFieldConfig& ef) {
    return EncryptionInformationHelpers::encryptionInformationSerialize(nss, ef.toBSON());
}

BSONObj EncryptionInformationHelpers::encryptionInformationSerialize(
    const NamespaceString& nss, const BSONObj& encryptedFields) {
    EncryptionInformation ei;
    ei.setType(kEncryptionInformationSchemaVersion);

    // Do not include tenant id in nss in the schema as the command request has unsigned security
    // token.
    ei.setSchema(BSON(nss.serializeWithoutTenantPrefix_UNSAFE() << encryptedFields));

    return ei.toBSON();
}

BSONObj EncryptionInformationHelpers::encryptionInformationSerialize(const BSONObj& schema) {
    EncryptionInformation ei;
    ei.setType(kEncryptionInformationSchemaVersion);
    ei.setSchema(schema);
    return ei.toBSON();
}

EncryptedFieldConfig EncryptionInformationHelpers::getAndValidateSchema(
    const NamespaceString& nss, const EncryptionInformation& ei) {
    BSONObj schema = ei.getSchema();

    // Do not include tenant id in nss in the schema as the command request has unsigned security
    // token.
    auto element = schema.getField(nss.serializeWithoutTenantPrefix_UNSAFE());

    uassert(6371205,
            "Expected an object for schema in EncryptionInformation",
            !element.eoo() && element.type() == BSONType::object);

    auto efc = EncryptedFieldConfig::parse(element.Obj(), IDLParserContext("schema"));

    uassert(6371207, "Expected a value for escCollection", efc.getEscCollection().has_value());
    uassert(6371208, "Expected a value for ecocCollection", efc.getEcocCollection().has_value());
    uassert(8575606,
            "Collection contains the 'rangePreview' query type which is deprecated. Please "
            "recreate the collection with the 'range' query type.",
            !hasQueryType(efc, QueryTypeEnum::RangePreviewDeprecated));
    return efc;
}

void EncryptionInformationHelpers::checkTagLimitsAndStorageNotExceeded(
    const EncryptedFieldConfig& ef) {

    auto calculateMaxTags = [](const boost::optional<StringData>& type,
                               StringData path,
                               const QueryTypeConfig& qtc) -> uint64_t {
        auto qtype = qtc.getQueryType();
        if (qtype == QueryTypeEnum::Equality) {
            return 1;
        } else if (qtype == QueryTypeEnum::Range) {
            uassert(10431801,
                    fmt::format("Missing bsonType for range field '{}'", path),
                    type.has_value());
            return getEdgesLength(typeFromName(type.value()), path, qtc);
        } else if (isFLE2TextQueryType(qtype)) {
            int32_t ub = qtc.getStrMaxQueryLength().get();
            int32_t lb = qtc.getStrMinQueryLength().get();
            if (qtype == QueryTypeEnum::SubstringPreview) {
                return maxTagsForSubstring(
                    lb, ub, static_cast<uint32_t>(qtc.getStrMaxLength().get()));
            }
            return maxTagsForSuffixOrPrefix(lb, ub);
        } else {
            uasserted(10431802, "Unknown or deprecated query type encountered");
        }
    };

    uint64_t totalTagCount = 0;
    for (const auto& field : ef.getFields()) {
        if (!field.getQueries()) {
            continue;
        }

        auto tagCount =
            visit(OverloadedVisitor{
                      [&](QueryTypeConfig qtc) {
                          return calculateMaxTags(field.getBsonType(), field.getPath(), qtc);
                      },
                      [&](std::vector<QueryTypeConfig> queries) {
                          uint64_t maxTags = 0;
                          for (auto& qtc : queries) {
                              maxTags +=
                                  calculateMaxTags(field.getBsonType(), field.getPath(), qtc);
                          }
                          return maxTags;
                      }},
                  field.getQueries().get());
        if (hasQueryTypeMatching(field, isFLE2TextQueryType)) {
            tagCount++;  // substring/suffix/prefix types get an extra tag for exact string match
            uassert(
                10384602,
                fmt::format("Queryable Encryption tag limit exceeded for field '{}'. Worst case "
                            "tag count is {}",
                            field.getPath(),
                            tagCount),
                tagCount <= kFLE2PerFieldTagLimit);
        }
        totalTagCount += tagCount;
    }

    auto totalTagStorage = (totalTagCount * kFLE2PerTagStorageBytes);
    auto shouldOverrideTotalTagOverheadLimit =
        ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<FLEOverrideTagOverheadData>>(
                "fleAllowTotalTagOverheadToExceedBSONLimit")
            ->getValue(boost::none)
            .getShouldOverride();
    uassert(
        10431800,
        fmt::format("Cannot create a collection where the worst case total Queryable Encryption "
                    "tag storage size ({}) exceeds the max BSON size ({}). Consider reducing the "
                    "number of encrypted fields in the schema, or tuning the indexing parameters.",
                    totalTagStorage,
                    BSONObjMaxUserSize),
        shouldOverrideTotalTagOverheadLimit || totalTagStorage <= BSONObjMaxUserSize);
}

void EncryptionInformationHelpers::checkSubstringPreviewParameterLimitsNotExceeded(
    const EncryptedFieldConfig& ef) {
    static_assert(kSubstringPreviewLowerBoundMin <= kSubstringPreviewUpperBoundMax);
    static_assert(kSubstringPreviewUpperBoundMax <= kSubstringPreviewMaxLengthMax);

    static constexpr StringData bypassMsg =
        "Consider setting the fleDisableSubstringPreviewParameterLimits cluster parameter to true "
        "to bypass this limit.";
    if (ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<FLEOverrideSubstringPreviewLimits>>(
                "fleDisableSubstringPreviewParameterLimits")
            ->getValue(boost::none)
            .getShouldOverride()) {
        return;
    }

    auto checkOneQueryType = [](StringData path, const QueryTypeConfig& qtc) {
        if (qtc.getQueryType() != QueryTypeEnum::SubstringPreview) {
            return;
        }
        int32_t ub = qtc.getStrMaxQueryLength().get();
        int32_t lb = qtc.getStrMinQueryLength().get();
        int32_t max = qtc.getStrMaxLength().get();
        uassert(10453200,
                fmt::format("strMinQueryLength ({}) must be >= {} for substringPreview query "
                            "type of field {}. {}",
                            lb,
                            kSubstringPreviewLowerBoundMin,
                            path,
                            bypassMsg),
                lb >= kSubstringPreviewLowerBoundMin);
        uassert(10453201,
                fmt::format("strMaxQueryLength ({}) must be >= {} for substringPreview query "
                            "type of field {}. {}",
                            ub,
                            kSubstringPreviewUpperBoundMax,
                            path,
                            bypassMsg),
                ub <= kSubstringPreviewUpperBoundMax);
        uassert(10453202,
                fmt::format("strMaxLength ({}) must be >= {} for substringPreview query "
                            "type of field {}. {}",
                            max,
                            kSubstringPreviewMaxLengthMax,
                            path,
                            bypassMsg),
                max <= kSubstringPreviewMaxLengthMax);
    };

    for (const auto& field : ef.getFields()) {
        if (!field.getQueries()) {
            continue;
        }

        visit(
            OverloadedVisitor{[&](QueryTypeConfig qtc) { checkOneQueryType(field.getPath(), qtc); },
                              [&](std::vector<QueryTypeConfig> queries) {
                                  for (auto& qtc : queries) {
                                      checkOneQueryType(field.getPath(), qtc);
                                  }
                              }},
            field.getQueries().get());
    }
}

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedConstDataRange(ConstDataRange cdr) {
    ConstDataRangeCursor cdrc(cdr);

    uint8_t subTypeByte = cdrc.readAndAdvance<uint8_t>();

    auto subType = EncryptedBinDataType_parse(subTypeByte, IDLParserContext("subtype"));
    return {subType, cdrc};
}

ParsedFindEqualityPayload::ParsedFindEqualityPayload(BSONElement fleFindPayload)
    : ParsedFindEqualityPayload(binDataToCDR(fleFindPayload)) {};

ParsedFindEqualityPayload::ParsedFindEqualityPayload(const Value& fleFindPayload)
    : ParsedFindEqualityPayload(binDataToCDR(fleFindPayload)) {};

ParsedFindEqualityPayload::ParsedFindEqualityPayload(ConstDataRange cdr) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
    auto encryptedType = encryptedTypeBinding;

    uassert(7292600,
            str::stream() << "Unexpected encrypted payload type: "
                          << static_cast<uint32_t>(encryptedType),
            encryptedType == EncryptedBinDataType::kFLE2FindEqualityPayloadV2);

    auto payload = parseFromCDR<FLE2FindEqualityPayloadV2>(subCdr);

    escToken = payload.getEscDerivedToken();
    edcToken = payload.getEdcDerivedToken();
    serverDataDerivedToken = payload.getServerDerivedFromDataToken();

    maxCounter = payload.getMaxCounter();
}

ParsedFindRangePayload::ParsedFindRangePayload(BSONElement fleFindPayload)
    : ParsedFindRangePayload(binDataToCDR(fleFindPayload)) {};

ParsedFindRangePayload::ParsedFindRangePayload(const Value& fleFindPayload)
    : ParsedFindRangePayload(binDataToCDR(fleFindPayload)) {};

ParsedFindRangePayload::ParsedFindRangePayload(ConstDataRange cdr) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
    auto encryptedType = encryptedTypeBinding;

    uassert(7292601,
            str::stream() << "Unexpected encrypted payload type: "
                          << static_cast<uint32_t>(encryptedType),
            encryptedType == EncryptedBinDataType::kFLE2FindRangePayloadV2);

    auto payload = parseFromCDR<FLE2FindRangePayloadV2>(subCdr);
    payloadId = payload.getPayloadId();
    firstOp = payload.getFirstOperator();
    secondOp = payload.getSecondOperator();
    precision = payload.getPrecision();
    trimFactor = payload.getTrimFactor();
    sparsity = payload.getSparsity();
    indexMin = payload.getIndexMin();
    indexMax = payload.getIndexMax();

    if (!payload.getPayload()) {
        return;
    }

    edges = std::vector<FLEFindEdgeTokenSet>();
    auto& edgesRef = edges.value();
    auto& info = payload.getPayload().value();

    for (auto const& edge : info.getEdges()) {
        edgesRef.push_back({edge.getEdcDerivedToken(),
                            edge.getEscDerivedToken(),
                            edge.getServerDerivedFromDataToken()});
    }

    maxCounter = info.getMaxCounter();
}

ParsedFindTextSearchPayload::ParsedFindTextSearchPayload(BSONElement fleFindPayload) {
    // We should never parse a BSONElement payload since we don't support match expressions.
    MONGO_UNREACHABLE_TASSERT(10112804);
};

ParsedFindTextSearchPayload::ParsedFindTextSearchPayload(const Value& fleFindPayload)
    : ParsedFindTextSearchPayload(binDataToCDR(fleFindPayload)) {};

ParsedFindTextSearchPayload::ParsedFindTextSearchPayload(ConstDataRange cdr) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
    auto encryptedType = encryptedTypeBinding;
    uassert(10112800,
            str::stream() << "Unexpected encrypted payload type: "
                          << static_cast<uint32_t>(encryptedType),
            encryptedType == EncryptedBinDataType::kFLE2FindTextPayload);

    auto payload = parseFromCDR<FLE2FindTextPayload>(subCdr);

    mongo::TextSearchFindTokenSets& tokens = payload.getTokenSets();

    // There must be only one of the following in the payload, previously verified.
    prefixTokens = tokens.getPrefixTokens();
    suffixTokens = tokens.getSuffixTokens();
    exactTokens = tokens.getExactTokens();
    substringTokens = tokens.getSubstringTokens();

    if (prefixTokens) {
        edc = EDCDerivedFromDataToken{prefixTokens->getEdcDerivedToken().asPrfBlock()};
        esc = ESCDerivedFromDataToken{prefixTokens->getEscDerivedToken().asPrfBlock()};
        server = ServerDerivedFromDataToken{prefixTokens->getServerDerivedToken().asPrfBlock()};
    } else if (suffixTokens) {
        edc = EDCDerivedFromDataToken{suffixTokens->getEdcDerivedToken().asPrfBlock()};
        esc = ESCDerivedFromDataToken{suffixTokens->getEscDerivedToken().asPrfBlock()};
        server = ServerDerivedFromDataToken{suffixTokens->getServerDerivedToken().asPrfBlock()};
    } else if (substringTokens) {
        edc = EDCDerivedFromDataToken{substringTokens->getEdcDerivedToken().asPrfBlock()};
        esc = ESCDerivedFromDataToken{substringTokens->getEscDerivedToken().asPrfBlock()};
        server = ServerDerivedFromDataToken{substringTokens->getServerDerivedToken().asPrfBlock()};
    } else {
        edc = EDCDerivedFromDataToken{exactTokens->getEdcDerivedToken().asPrfBlock()};
        esc = ESCDerivedFromDataToken{exactTokens->getEscDerivedToken().asPrfBlock()};
        server = ServerDerivedFromDataToken{exactTokens->getServerDerivedToken().asPrfBlock()};
    }

    maxCounter = payload.getMaxCounter();
}


std::vector<CompactionToken> CompactionHelpers::parseCompactionTokens(BSONObj compactionTokens) {
    std::vector<CompactionToken> parsed;
    std::transform(
        compactionTokens.begin(),
        compactionTokens.end(),
        std::back_inserter(parsed),
        [](const auto& token) {
            auto fieldName = std::string{token.fieldNameStringData()};

            if (token.isBinData(BinDataType::BinDataGeneral)) {
                auto ecoc = ECOCToken::parse(token._binDataVector());
                return CompactionToken{std::move(fieldName), std::move(ecoc), boost::none};
            }

            if (token.type() == BSONType::object) {
                auto doc =
                    CompactionTokenDoc::parse(token.Obj(), IDLParserContext{"compactionToken"});
                return CompactionToken{
                    std::move(fieldName), doc.getECOCToken(), doc.getAnchorPaddingToken()};
            }

            uasserted(6346801,
                      fmt::format("Field '{}' of compaction tokens must be a BinData(General) or "
                                  "Object, got '{}'",
                                  fieldName,
                                  typeName(token.type())));
        });
    return parsed;
}

void CompactionHelpers::validateCompactionOrCleanupTokens(const EncryptedFieldConfig& efc,
                                                          BSONObj compactionTokens,
                                                          StringData tokenType) {
    _validateTokens(efc, compactionTokens, tokenType);
}

void CompactionHelpers::_validateTokens(const EncryptedFieldConfig& efc,
                                        BSONObj tokens,
                                        StringData cmd) {
    for (const auto& field : efc.getFields()) {
        const auto& tokenElement = tokens.getField(field.getPath());
        uassert(7294900,
                str::stream() << cmd << " tokens object is missing " << cmd
                              << " token for the encrypted path '" << field.getPath() << "'",
                !tokenElement.eoo());
    }
}

ConstDataRange binDataToCDR(BSONElement element) {
    uassert(6338501, "Expected binData BSON element", element.type() == BSONType::binData);

    int len;
    const char* data = element.binData(len);
    return ConstDataRange(data, data + len);
}

bool hasQueryTypeMatching(const EncryptedField& field, const QueryTypeMatchFn& matcher) {
    if (!field.getQueries()) {
        return false;
    }
    return visit(OverloadedVisitor{
                     [&](QueryTypeConfig query) { return matcher(query.getQueryType()); },
                     [&](std::vector<QueryTypeConfig> queries) {
                         return std::any_of(
                             queries.cbegin(), queries.cend(), [&](const QueryTypeConfig& qtc) {
                                 return matcher(qtc.getQueryType());
                             });
                     }},
                 field.getQueries().get());
}
bool hasQueryTypeMatching(const EncryptedFieldConfig& config, const QueryTypeMatchFn& matcher) {
    for (const auto& field : config.getFields()) {
        if (field.getQueries().has_value()) {
            bool hasQuery = hasQueryTypeMatching(field, matcher);
            if (hasQuery) {
                return hasQuery;
            }
        }
    }
    return false;
}

bool hasQueryType(const EncryptedField& field, QueryTypeEnum queryType) {
    return hasQueryTypeMatching(field, [queryType](QueryTypeEnum qt) { return qt == queryType; });
}

bool hasQueryType(const EncryptedFieldConfig& config, QueryTypeEnum queryType) {
    return hasQueryTypeMatching(config, [queryType](QueryTypeEnum qt) { return qt == queryType; });
}

QueryTypeConfig getQueryType(const EncryptedField& field, QueryTypeEnum queryType) {
    uassert(8574703,
            fmt::format("Field '{}' is missing a QueryTypeConfig", field.getPath()),
            field.getQueries());

    return visit(OverloadedVisitor{
                     [&](QueryTypeConfig query) {
                         uassert(8574704,
                                 fmt::format("Field '{}' should be of type '{}', got '{}'",
                                             field.getPath(),
                                             QueryType_serializer(queryType),
                                             QueryType_serializer(query.getQueryType())),
                                 query.getQueryType() == queryType);
                         return query;
                     },
                     [&](std::vector<QueryTypeConfig> queries) {
                         for (const auto& query : queries) {
                             if (query.getQueryType() == queryType) {
                                 return query;
                             }
                         }
                         uasserted(
                             8674705,
                             fmt::format("Field '{}' should be of type '{}', but no configs match",
                                         field.getPath(),
                                         QueryType_serializer(queryType)));
                     }},
                 field.getQueries().get());
}

EncryptedPredicateEvaluatorV2::EncryptedPredicateEvaluatorV2(
    std::vector<ServerZerosEncryptionToken> zerosTokens)
    : _zerosDecryptionTokens(std::move(zerosTokens)) {};

bool EncryptedPredicateEvaluatorV2::evaluate(
    Value fieldValue,
    EncryptedBinDataType indexedValueType,
    std::function<std::vector<ConstFLE2TagAndEncryptedMetadataBlock>(ConstDataRange)>
        extractMetadataBlocks) const {

    if (fieldValue.getType() != BSONType::binData) {
        return false;
    }

    auto [subSubType, data] = fromEncryptedBinData(fieldValue);

    uassert(7399501, "Invalid encrypted indexed field", subSubType == indexedValueType);

    auto binData = fieldValue.getBinData();
    std::vector<ConstFLE2TagAndEncryptedMetadataBlock> metadataBlocks =
        extractMetadataBlocks(binDataToCDR(binData));

    for (const auto& zeroDecryptionToken : _zerosDecryptionTokens) {
        for (auto& metadataBlock : metadataBlocks) {
            auto swZerosBlob = metadataBlock.decryptZerosBlob(zeroDecryptionToken);
            uassertStatusOK(swZerosBlob);
            if (FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(swZerosBlob.getValue())) {
                return true;
            }
        }
    }

    return false;
}

// Edges

namespace {
int resolveTrimFactorDefault(int maxlen, const boost::optional<int>& optTrimFactor) {
    if (optTrimFactor) {
        return *optTrimFactor;
    }

    return std::clamp(kFLERangeTrimFactorDefault, 0, maxlen - 1);
}
}  // namespace

Edges::Edges(std::string leaf, int sparsity, const boost::optional<int>& optTrimFactor)
    : _leaf(std::move(leaf)),
      _sparsity(sparsity),
      _trimFactor(resolveTrimFactorDefault(_leaf.length(), optTrimFactor)) {
    uassert(6775101, "sparsity must be 1 or larger", _sparsity > 0);
    dassert(std::all_of(_leaf.begin(), _leaf.end(), [](char c) { return c == '1' || c == '0'; }));
    uassert(8574105,
            "trim factor must be >= 0 and less than the number of bits used to represent an "
            "element of the domain",
            _trimFactor >= 0 && (_trimFactor == 0 || (size_t)_trimFactor < _leaf.length()));
}

std::vector<StringData> Edges::get() {
    static const StringData kRoot = "root"_sd;
    StringData leaf = _leaf;
    std::vector<StringData> result;
    if (_trimFactor == 0) {
        result.push_back(kRoot);
    }
    result.push_back(leaf);

    size_t startLevel = _trimFactor < 1 ? 1 : _trimFactor;
    for (size_t i = startLevel; i < _leaf.size(); ++i) {
        if (i % _sparsity == 0) {
            result.push_back(leaf.substr(0, i));
        }
    }
    return result;
}

std::size_t Edges::size() const {
    // Edges::get() generates, starting from the highest untrimmed level, every {sparsity}'th chunk
    // of leaf (counting from the original root), with the full leaf being a guaranteed capture,
    // regardless of sparsity.

    // When trimFactor == 0 or 1, we trim nothing or just the root; when > 1, we trim (TF - 1)
    // non-root levels.
    std::size_t trimmedNonRootLevels = _trimFactor > 1 ? _trimFactor - 1 : 0;
    // Count total number of edges (not accounting for trimming) which would be included according
    // to sparsity, then remove trimmed edges which would have been included.
    std::size_t edges = (_leaf.size() / _sparsity) - (trimmedNonRootLevels / _sparsity);
    if ((_leaf.size() % _sparsity) != 0) {
        // Always capture full leaf in the count.
        ++edges;
    }
    // Add the root edge if it is not trimmed.
    return (_trimFactor == 0 ? 1 : 0) + edges;
}

template <typename T>
std::unique_ptr<Edges> getEdgesT(
    T value, T min, T max, int sparsity, const boost::optional<int>& trimFactor) {
    static_assert(!std::numeric_limits<T>::is_signed);
    static_assert(std::numeric_limits<T>::is_integer);

    constexpr size_t bits = std::numeric_limits<T>::digits;

    dassert(0 == min);

    size_t maxlen = getFirstBitSet(max);
    std::string valueBin = toBinaryString(value);
    std::string valueBinTrimmed = valueBin.substr(bits - maxlen, maxlen);
    return std::make_unique<Edges>(valueBinTrimmed, sparsity, trimFactor);
}

std::unique_ptr<Edges> getEdgesInt32(int32_t value,
                                     boost::optional<int32_t> min,
                                     boost::optional<int32_t> max,
                                     int sparsity,
                                     const boost::optional<int>& trimFactor) {
    auto aost = getTypeInfo32(value, min, max);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity, trimFactor);
}

std::unique_ptr<Edges> getEdgesInt64(int64_t value,
                                     boost::optional<int64_t> min,
                                     boost::optional<int64_t> max,
                                     int sparsity,
                                     const boost::optional<int>& trimFactor) {
    auto aost = getTypeInfo64(value, min, max);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity, trimFactor);
}

std::unique_ptr<Edges> getEdgesDouble(double value,
                                      boost::optional<double> min,
                                      boost::optional<double> max,
                                      boost::optional<uint32_t> precision,
                                      int sparsity,
                                      const boost::optional<int>& trimFactor) {
    auto aost = getTypeInfoDouble(value, min, max, precision);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity, trimFactor);
}

std::unique_ptr<Edges> getEdgesDecimal128(Decimal128 value,
                                          boost::optional<Decimal128> min,
                                          boost::optional<Decimal128> max,
                                          boost::optional<uint32_t> precision,
                                          int sparsity,
                                          const boost::optional<int>& trimFactor) {
    auto aost = getTypeInfoDecimal128(value, min, max, precision);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity, trimFactor);
}

std::uint64_t getEdgesLength(BSONType fieldType, StringData fieldPath, QueryTypeConfig config) {
    // validates fieldType & config and sets defaults
    setRangeDefaults(fieldType, fieldPath, &config);

    const auto sparsity = *config.getSparsity();
    const auto trimFactor = config.getTrimFactor();
    auto precision = config.getPrecision().map(
        [](auto signedInt) -> uint32_t { return static_cast<uint32_t>(signedInt); });

    switch (fieldType) {
        case BSONType::numberInt: {
            auto min = config.getMin()->getInt();
            return getEdgesInt32(min, min, config.getMax()->getInt(), sparsity, trimFactor)->size();
        }
        case BSONType::numberLong: {
            auto min = config.getMin()->getLong();
            return getEdgesInt64(min, min, config.getMax()->getLong(), sparsity, trimFactor)
                ->size();
        }
        case BSONType::numberDouble: {
            auto min = config.getMin()->getDouble();
            return getEdgesDouble(
                       min, min, config.getMax()->getDouble(), precision, sparsity, trimFactor)
                ->size();
        }
        case BSONType::numberDecimal: {
            auto min = config.getMin()->getDecimal();
            return getEdgesDecimal128(
                       min, min, config.getMax()->getDecimal(), precision, sparsity, trimFactor)
                ->size();
        }
        case BSONType::date: {
            auto min = config.getMin()->getDate().toMillisSinceEpoch();
            return getEdgesInt64(min,
                                 min,
                                 config.getMax()->getDate().toMillisSinceEpoch(),
                                 sparsity,
                                 trimFactor)
                ->size();
        }
        default:
            uasserted(8674710,
                      fmt::format("Invalid queryTypeConfig.type '{}'", typeName(fieldType)));
    }

    MONGO_UNREACHABLE;
}

template <typename T>
class MinCoverGenerator {
public:
    static std::vector<std::string> minCover(
        T lowerBound, T upperBound, T max, int sparsity, const boost::optional<int>& trimFactor) {
        MinCoverGenerator<T> mcg(lowerBound, upperBound, max, sparsity, trimFactor);
        std::vector<std::string> c;
        mcg.minCoverRec(c, 0, mcg._maxlen);
        return c;
    }

private:
    MinCoverGenerator(
        T lowerBound, T upperBound, T max, int sparsity, const boost::optional<int>& optTrimFactor)
        : _lowerBound(lowerBound),
          _upperBound(upperBound),
          _sparsity(sparsity),
          _maxlen(getFirstBitSet(max)),
          _trimFactor(resolveTrimFactorDefault(_maxlen, optTrimFactor)) {
        static_assert(!std::numeric_limits<T>::is_signed);
        static_assert(std::numeric_limits<T>::is_integer);
        tassert(6860001,
                "Lower bound must be less or equal to upper bound for range search.",
                lowerBound <= upperBound);
        dassert(lowerBound >= 0 && upperBound <= max);
        uassert(8574106,
                "Trim factor must be >= 0 and less than the number of bits used to represent an "
                "element of the domain",
                _trimFactor >= 0 && (_trimFactor == 0 || _trimFactor < _maxlen));
    }

    // Generate and apply a mask to an integer, filling masked bits with 1;
    // bits from 0 to bits-1 will be set to 1. Other bits are left as-is.
    // for example: applyMask(0b00000000, 4) = 0b00001111
    static T applyMask(T value, int maskedBits) {
        constexpr T ones = ~static_cast<T>(0);

        invariant(maskedBits <= std::numeric_limits<T>::digits);
        invariant(maskedBits >= 0);

        if (maskedBits == 0) {
            return value;
        }

        const T mask = ones >> (std::numeric_limits<T>::digits - maskedBits);
        return value | mask;
    }

    // Some levels are discarded when sparsity does not divide current level, or when they are
    // trimmed when trim factor is greater than the current level Discarded levels are replaced by
    // the set of edges on the next level Return true if level is stored
    bool isLevelStored(int maskedBits) {
        int level = _maxlen - maskedBits;
        return 0 == maskedBits || (level >= _trimFactor && 0 == (level % _sparsity));
    }

    std::string toString(T start, int maskedBits) {
        constexpr size_t bits = std::numeric_limits<T>::digits;
        dassert(maskedBits <= _maxlen);
        if (maskedBits == _maxlen) {
            return "root";
        }
        std::string valueBin = toBinaryString(start >> maskedBits);
        return valueBin.substr(bits - _maxlen + maskedBits, _maxlen - maskedBits);
    }

    // Generate a minCover recursively for the minimum set of edges covered
    // by [_rangeMin, _rangeMax]. Edges on a level discarded due to sparsity are
    // replaced with the edges from next level
    void minCoverRec(std::vector<std::string>& c, T blockStart, int maskedBits) {
        const T blockEnd = applyMask(blockStart, maskedBits);

        if (blockEnd < _lowerBound || blockStart > _upperBound) {
            return;
        }

        if (blockStart >= _lowerBound && blockEnd <= _upperBound && isLevelStored(maskedBits)) {
            c.push_back(toString(blockStart, maskedBits));
            return;
        }

        invariant(maskedBits > 0);

        const int newBits = maskedBits - 1;
        minCoverRec(c, blockStart, newBits);
        minCoverRec(c, blockStart | (static_cast<T>(1) << newBits), newBits);
    }

private:
    T _lowerBound;
    T _upperBound;
    int _sparsity;
    int _maxlen;
    int _trimFactor;
};

template <typename T>
std::vector<std::string> minCover(T lowerBound,
                                  T upperBound,
                                  T min,
                                  T max,
                                  int sparsity,
                                  const boost::optional<int>& trimFactor) {
    dassert(0 == min);
    return MinCoverGenerator<T>::minCover(lowerBound, upperBound, max, sparsity, trimFactor);
}

/**
 * OST-1 represents all ranges as inclusive, but MQL queries also have support for ranges that
 * exclude either bound. If the user query does not include the lower/upper bound, then narrow the
 * range by 1 on the proper end.
 *
 * This function is templated so that it can operate on the concrete OSTType struct for each
 * supported numeric type.
 */
template <typename T>
void adjustBounds(T& lowerBound, bool includeLowerBound, T& upperBound, bool includeUpperBound) {
    if (!includeLowerBound) {
        uassert(6901316,
                "Lower bound must be less than the range maximum if lower bound is excluded from "
                "range.",
                lowerBound.value < lowerBound.max);
        lowerBound.value += 1;
    }
    if (!includeUpperBound) {
        uassert(6901317,
                "Upper bound must be greater than the range minimum if upper bound is excluded "
                "from range.",
                upperBound.value > upperBound.min);
        upperBound.value -= 1;
    }
}

std::vector<std::string> minCoverInt32(int32_t lowerBound,
                                       bool includeLowerBound,
                                       int32_t upperBound,
                                       bool includeUpperBound,
                                       boost::optional<int32_t> min,
                                       boost::optional<int32_t> max,
                                       int sparsity,
                                       const boost::optional<int>& trimFactor) {
    auto a = getTypeInfo32(lowerBound, min, max);
    auto b = getTypeInfo32(upperBound, min, max);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity, trimFactor);
}

std::vector<std::string> minCoverInt64(int64_t lowerBound,
                                       bool includeLowerBound,
                                       int64_t upperBound,
                                       bool includeUpperBound,
                                       boost::optional<int64_t> min,
                                       boost::optional<int64_t> max,
                                       int sparsity,
                                       const boost::optional<int>& trimFactor) {
    auto a = getTypeInfo64(lowerBound, min, max);
    auto b = getTypeInfo64(upperBound, min, max);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity, trimFactor);
}

std::vector<std::string> minCoverDouble(double lowerBound,
                                        bool includeLowerBound,
                                        double upperBound,
                                        bool includeUpperBound,
                                        boost::optional<double> min,
                                        boost::optional<double> max,
                                        boost::optional<uint32_t> precision,
                                        int sparsity,
                                        const boost::optional<int>& trimFactor) {
    auto a = getTypeInfoDouble(lowerBound, min, max, precision);
    auto b = getTypeInfoDouble(upperBound, min, max, precision);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity, trimFactor);
}
std::vector<std::string> minCoverDecimal128(Decimal128 lowerBound,
                                            bool includeLowerBound,
                                            Decimal128 upperBound,
                                            bool includeUpperBound,
                                            boost::optional<Decimal128> min,
                                            boost::optional<Decimal128> max,
                                            boost::optional<uint32_t> precision,
                                            int sparsity,
                                            const boost::optional<int>& trimFactor) {
    auto a = getTypeInfoDecimal128(lowerBound, min, max, precision);
    auto b = getTypeInfoDecimal128(upperBound, min, max, precision);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity, trimFactor);
}

namespace {
int32_t calculatePaddedLengthForString(int32_t strLen) {
    // See
    // https://github.com/10gen/mongo/blob/master/src/mongo/db/modules/enterprise/docs/fle/fle_string_search.md#strencode-substring
    // for an explanation of the padlen calculation below.
    dassert(strLen >= 0);
    static constexpr int32_t kBSONStringOverheadBytes = 5;  // 4-byte size + null terminator byte

    uassert(10384600,
            fmt::format(
                "String length {} is too long for substring/suffix/prefix indexed encrypted field",
                strLen),
            strLen <= std::numeric_limits<int32_t>::max() - kBSONStringOverheadBytes - 15);

    // round strLen + overhead to the nearest 16-byte boundary
    int32_t padLen = ((strLen + kBSONStringOverheadBytes + 15) / 16) * 16;

    // readjust for BSON overhead
    padLen -= kBSONStringOverheadBytes;
    return padLen;
}

uint32_t calculateMsize(int32_t strLen, int32_t lb, int32_t ub) {
    // OST calculates the substring tag count (msize) generally as:
    //
    //    msize = Summation(j=[lb...ub], (strLen-j+1))
    //
    // where lb and ub are the shortest and longest substring lengths to index,
    // respectively, and strLen is the length of the string to index.
    //
    // For each j, the value of (strLen-j+1) is just one less than the previous value.
    // So, this can be rewritten as:
    //
    //    msize = Summation(j=[(strLen-ub+1)...(strLen-lb+1)], j)
    //
    // i.e. sum of the arithmetic sequence [(strLen-ub+1) ... (strLen-lb+1)],
    // which can be simply calculated using the formula sum = (a1 + a2) * n/2
    dassert(lb > 0);
    dassert(ub >= lb);
    dassert(strLen >= lb);

    // # of substrings of length ub from a string of length strLen
    int32_t largestSubstrCount = strLen - ub + 1;
    // # of substrings of length lb from a string of length strLen
    int32_t smallestSubstrCount = strLen - lb + 1;

    // Do the arithmetic as uint64_t to avoid overflows.
    // (a1 + a2) * n will not exceed UINT64_MAX even if all variables are INT32_MAX.
    uint64_t a1 = static_cast<uint64_t>(largestSubstrCount);
    uint64_t a2 = static_cast<uint64_t>(smallestSubstrCount);
    uint64_t n = smallestSubstrCount - largestSubstrCount + 1;
    uint64_t sum = (a1 + a2) * n / 2;  // always evenly divisible
    uassert(10384601,
            fmt::format(
                "Calculated tag count {} is too large for substring indexed encrypted field", sum),
            sum <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
    return static_cast<uint32_t>(sum);
}
}  // namespace

uint32_t msizeForSubstring(int32_t strLen, int32_t lb, int32_t ub, int32_t mlen) {
    dassert(lb > 0);
    dassert(ub >= lb);
    dassert(mlen >= ub);

    auto padLen = calculatePaddedLengthForString(strLen);
    if (lb > padLen) {
        return 0;
    }

    padLen = std::min(mlen, padLen);  // cap padLen to mlen
    ub = std::min(padLen, ub);        // cap ub to padLen (i.e. if padLen < ub)
    return calculateMsize(padLen, lb, ub);
}

uint32_t msizeForSuffixOrPrefix(int32_t strLen, int32_t lb, int32_t ub) {
    dassert(lb > 0);
    dassert(ub >= lb);

    auto padLen = calculatePaddedLengthForString(strLen);
    if (lb > padLen) {
        return 0;
    }
    return static_cast<uint32_t>(std::min(padLen, ub) - lb + 1);
}

uint32_t maxTagsForSubstring(int32_t lb, int32_t ub, int32_t mlen) {
    dassert(mlen >= ub);
    // Worst case tag count for substring is calculated in OST as:
    //     max = Summation(j=[lb...ub], (mlen-j+1))
    return calculateMsize(mlen, lb, ub);
}

uint32_t maxTagsForSuffixOrPrefix(int32_t lb, int32_t ub) {
    dassert(lb > 0);
    dassert(ub >= lb);
    return static_cast<uint32_t>(ub - lb + 1);
}

PrfBlock FLEUtil::blockToArray(const SHA256Block& block) {
    PrfBlock data;
    memcpy(data.data(), block.data(), sizeof(PrfBlock));
    return data;
}

PrfBlock FLEUtil::prf(HmacContext* context, ConstDataRange key, uint64_t value, int64_t value2) {
    uassert(6378003, "Invalid key length", key.length() == crypto::sym256KeySize);

    SHA256Block block;

    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);


    std::array<char, sizeof(uint64_t)> bufValue2;
    DataView(bufValue2.data()).write<LittleEndian<uint64_t>>(value2);

    SHA256Block::computeHmacWithCtx(context,
                                    key.data<uint8_t>(),
                                    key.length(),
                                    {
                                        ConstDataRange{bufValue},
                                        ConstDataRange{bufValue2},
                                    },
                                    &block);
    return FLEUtil::blockToArray(block);
}

PrfBlock FLEUtil::prf(HmacContext* hmacCtx, ConstDataRange key, ConstDataRange cdr) {
    uassert(6378002, "Invalid key length", key.length() == crypto::sym256KeySize);

    SHA256Block block;
    SHA256Block::computeHmacWithCtx(hmacCtx, key.data<uint8_t>(), key.length(), {cdr}, &block);
    return blockToArray(block);
}

PrfBlock FLEUtil::prf(HmacContext* hmacCtx, ConstDataRange key, uint64_t value) {
    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);

    return prf(hmacCtx, key, bufValue);
}

StatusWith<std::vector<uint8_t>> FLEUtil::decryptData(ConstDataRange key,
                                                      ConstDataRange cipherText) {
    auto plainTextLength = fle2GetPlainTextLength(cipherText.length());
    if (!plainTextLength.isOK()) {
        return plainTextLength.getStatus();
    }

    std::vector<uint8_t> out(static_cast<size_t>(plainTextLength.getValue()));

    auto status = crypto::fle2Decrypt(key, cipherText, out);
    if (!status.isOK()) {
        return status.getStatus();
    }

    return {out};
}

StatusWith<std::vector<uint8_t>> FLEUtil::encryptData(ConstDataRange key,
                                                      ConstDataRange plainText) {
    MongoCryptStatus status;
    // AES-256-CTR
    auto* fle2alg = _mcFLE2Algorithm();
    auto ciphertextLen = fle2alg->get_ciphertext_len(plainText.length(), status);
    if (!status.isOK()) {
        return status.toStatus();
    }
    MongoCryptBuffer out;
    out.resize(ciphertextLen);

    MongoCryptBuffer iv;
    iv.resize(MONGOCRYPT_IV_LEN);
    auto* crypto = getGlobalMongoCrypt()->crypto;
    if (!_mongocrypt_random(crypto, iv.get(), MONGOCRYPT_IV_LEN, status)) {
        return status.toStatus();
    }

    uint32_t written;
    if (!fle2alg->do_encrypt(crypto,
                             iv.get() /* iv */,
                             NULL /* aad */,
                             MongoCryptBuffer::borrow(key).get(),
                             MongoCryptBuffer::borrow(plainText).get(),
                             out.get(),
                             &written,
                             status)) {
        return status.toStatus();
    }

    auto cdr = out.toCDR();
    return std::vector<uint8_t>(cdr.data(), cdr.data() + cdr.length());
}

template class ESCCollectionCommon<ESCTwiceDerivedTagToken, ESCTwiceDerivedValueToken>;
template class ESCCollectionCommon<AnchorPaddingKeyToken, AnchorPaddingValueToken>;
}  // namespace mongo
