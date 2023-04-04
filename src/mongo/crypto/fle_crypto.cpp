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
#include <boost/multiprecision/cpp_int.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <stack>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

extern "C" {
#include <mc-fle2-payload-iev-private.h>
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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_crypto_predicate.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_fields_util.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/random.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

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
constexpr uint64_t kECC = 3;
constexpr uint64_t kECOC = 4;


constexpr uint64_t kTwiceDerivedTokenFromEDC = 1;
constexpr uint64_t kTwiceDerivedTokenFromESCTag = 1;
constexpr uint64_t kTwiceDerivedTokenFromESCValue = 2;
constexpr uint64_t kTwiceDerivedTokenFromECCTag = 1;
constexpr uint64_t kTwiceDerivedTokenFromECCValue = 2;

constexpr uint64_t kServerCountAndContentionFactorEncryption = 1;
constexpr uint64_t kServerZerosEncryption = 2;

constexpr int32_t kEncryptionInformationSchemaVersion = 1;

constexpr auto kECCNullId = 0;
constexpr auto kECCNonNullId = 1;
constexpr uint64_t kECCompactionRecordValue = std::numeric_limits<uint64_t>::max();

constexpr uint64_t kESCNullId = 0;
constexpr uint64_t kESCNonNullId = 1;

constexpr uint64_t KESCInsertRecordValue = 0;
constexpr uint64_t kESCompactionRecordValue = std::numeric_limits<uint64_t>::max();

constexpr uint64_t kESCAnchorId = 0;
constexpr uint64_t kESCNullAnchorPosition = 0;
constexpr uint64_t kESCNonNullAnchorValuePrefix = 0;

constexpr auto kId = "_id";
constexpr auto kValue = "value";
constexpr auto kFieldName = "fieldName";

constexpr auto kDollarPush = "$push";
constexpr auto kDollarPull = "$pull";
constexpr auto kDollarEach = "$each";
constexpr auto kDollarIn = "$in";

constexpr auto kEncryptedFields = "encryptedFields";

constexpr size_t kHmacKeyOffset = 64;

constexpr boost::multiprecision::uint128_t k1(1);
constexpr boost::multiprecision::int128_t k10(10);
constexpr boost::multiprecision::uint128_t k10ui(10);

#ifdef FLE2_DEBUG_STATE_COLLECTIONS
constexpr auto kDebugId = "_debug_id";
constexpr auto kDebugValuePosition = "_debug_value_position";
constexpr auto kDebugValueCount = "_debug_value_count";

constexpr auto kDebugValueStart = "_debug_value_start";
constexpr auto kDebugValueEnd = "_debug_value_end";
#endif

namespace libmongocrypt_support_detail {

template <typename T>
using libmongocrypt_deleter_func = void(T*);

template <typename T, libmongocrypt_deleter_func<T> DelFunc>
struct LibMongoCryptDeleter {
    void operator()(T* ptr) {
        if (ptr) {
            DelFunc(ptr);
        }
    }
};

template <typename T, libmongocrypt_deleter_func<T> DelFunc>
using libmongocrypt_unique_ptr = std::unique_ptr<T, LibMongoCryptDeleter<T, DelFunc>>;

}  // namespace libmongocrypt_support_detail

using UniqueMongoCrypt =
    libmongocrypt_support_detail::libmongocrypt_unique_ptr<mongocrypt_t, mongocrypt_destroy>;

using UniqueMongoCryptCtx =
    libmongocrypt_support_detail::libmongocrypt_unique_ptr<mongocrypt_ctx_t,
                                                           mongocrypt_ctx_destroy>;

/**
 * C++ friendly wrapper around libmongocrypt's public mongocrypt_status_t* and its associated
 * functions.
 */
class MongoCryptStatus {
public:
    ~MongoCryptStatus() {
        mongocrypt_status_destroy(_status);
    }

    MongoCryptStatus(MongoCryptStatus&) = delete;
    MongoCryptStatus(MongoCryptStatus&&) = delete;

    static MongoCryptStatus create() {
        return MongoCryptStatus(mongocrypt_status_new());
    }

    /**
     * Get the error category for the status.
     */
    mongocrypt_status_type_t getType() {
        return mongocrypt_status_type(_status);
    }

    /**
     * Get a libmongocrypt specific error code
     */
    uint32_t getCode() {
        return mongocrypt_status_code(_status);
    }

    /**
     * Returns true if there are no errors
     */
    bool isOk() {
        return mongocrypt_status_ok(_status);
    }

    operator mongocrypt_status_t*() {
        return _status;
    }

    /**
     * Convert a mongocrypt_status_t to a mongo::Status.
     */
    Status toStatus() {
        if (getType() == MONGOCRYPT_STATUS_OK) {
            return Status::OK();
        }

        uint32_t len;
        StringData errorPrefix;
        switch (getType()) {
            case MONGOCRYPT_STATUS_ERROR_CLIENT: {
                errorPrefix = "Client Error: "_sd;
                break;
            }
            case MONGOCRYPT_STATUS_ERROR_KMS: {
                errorPrefix = "Kms Error: "_sd;
                break;
            }
            case MONGOCRYPT_STATUS_ERROR_CRYPT_SHARED: {
                errorPrefix = "Crypt Shared  Error: "_sd;
                break;
            }
            default: {
                errorPrefix = "Unexpected Error: "_sd;
                break;
            }
        }

        return Status(ErrorCodes::LibmongocryptError,
                      str::stream() << errorPrefix << mongocrypt_status_message(_status, &len));
    }

private:
    explicit MongoCryptStatus(mongocrypt_status_t* status) : _status(status) {}

private:
    mongocrypt_status_t* _status;
};

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

/**
 * C++ friendly wrapper around libmongocrypt's private _mongocrypt_buffer_t and its associated
 * functions.
 *
 * This class may or may not own data.
 */
class MongoCryptBuffer {
public:
    MongoCryptBuffer() {
        _mongocrypt_buffer_init(&_buffer);
    }

    MongoCryptBuffer(ConstDataRange cdr) {
        _mongocrypt_buffer_init(&_buffer);

        _buffer.data = const_cast<uint8_t*>(cdr.data<uint8_t>());
        _buffer.len = cdr.length();
    }

    ~MongoCryptBuffer() {
        _mongocrypt_buffer_cleanup(&_buffer);
    }

    MongoCryptBuffer(MongoCryptBuffer&) = delete;
    MongoCryptBuffer(MongoCryptBuffer&&) = delete;

    uint32_t length() {
        return _buffer.len;
    }

    uint8_t* data() {
        return _buffer.data;
    }

    bool empty() {
        return _mongocrypt_buffer_empty(&_buffer);
    }

    ConstDataRange toCDR() {
        return ConstDataRange(data(), length());
    }

private:
    _mongocrypt_buffer_t _buffer;
};

using UUIDBuf = std::array<uint8_t, UUID::kNumBytes>;

static_assert(sizeof(PrfBlock) == SHA256Block::kHashLength);

ConstDataRange hmacKey(const KeyMaterial& keyMaterial) {
    static_assert(kHmacKeyOffset + crypto::sym256KeySize <= crypto::kFieldLevelEncryptionKeySize);
    invariant(crypto::kFieldLevelEncryptionKeySize == keyMaterial->size());
    return {keyMaterial->data() + kHmacKeyOffset, crypto::sym256KeySize};
}


PrfBlock prf(ConstDataRange key, uint64_t value, int64_t value2) {
    uassert(6378003, "Invalid key length", key.length() == crypto::sym256KeySize);

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
    return FLEUtil::blockToArray(block);
}


ConstDataRange binDataToCDR(const BSONBinData binData) {
    int len = binData.length;
    const char* data = static_cast<const char*>(binData.data);
    return ConstDataRange(data, data + len);
}

ConstDataRange binDataToCDR(const Value& value) {
    uassert(6334103, "Expected binData Value type", value.getType() == BinData);

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

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedBinData(const BSONElement element) {
    uassert(
        6672414, "Expected binData with subtype Encrypt", element.isBinData(BinDataType::Encrypt));

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

StatusWith<std::vector<uint8_t>> encryptData(ConstDataRange key, ConstDataRange plainText) {
    std::vector<uint8_t> out(crypto::fle2CipherOutputLength(plainText.length()));

    auto status = crypto::fle2Encrypt(key, plainText, ConstDataRange(0, 0), out);
    if (!status.isOK()) {
        return status;
    }

    return {out};
}

StatusWith<std::vector<uint8_t>> encryptData(ConstDataRange key, uint64_t value) {

    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);

    return encryptData(key, bufValue);
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


template <typename collectionT, typename tagTokenT, typename valueTokenT>
boost::optional<uint64_t> emuBinaryCommon(const FLEStateCollectionReader& reader,
                                          tagTokenT tagToken,
                                          valueTokenT valueToken) {

    auto tracker = FLEStatusSection::get().makeEmuBinaryTracker();

    // Default search parameters
    uint64_t lambda = 0;
    boost::optional<uint64_t> i = 0;

    // Step 2:
    // Search for null record
    PrfBlock nullRecordId = collectionT::generateId(tagToken, boost::none);

    BSONObj nullDoc = reader.getById(nullRecordId);

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
    uint64_t rho = reader.getDocumentCount();

    // Since fast count() is not reliable, if it says zero, try 1 instead just to be sure the
    // collection is empty.
    if (rho == 0) {
        rho = 1;
    }

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
        bool docExists = reader.existsById(collectionT::generateId(tagToken, rho + lambda));

#ifdef DEBUG_ENUM_BINARY
        std::cout << fmt::format("search1: rho: {},  doc: {}", rho, doc.toString()) << std::endl;
#endif

        // 7 b
        if (docExists) {
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
        tracker.recordSuboperation();
        // 9a
        median = ceil(static_cast<double>(max - min) / 2) + min;


        // 9b
        bool docExists = reader.existsById(collectionT::generateId(tagToken, median + lambda));

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
        if (docExists) {
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
                bool docExists2 = reader.existsById(collectionT::generateId(tagToken, 1 + lambda));
                // 9 d ii B
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

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey.key);
    auto serverEncryptToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(indexKey.key);
    auto serverDerivationToken =
        FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(indexKey.key);

    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collectionToken);
    auto serverDerivedFromDataToken =
        FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(serverDerivationToken,
                                                                             value);
    EDCDerivedFromDataToken edcDataToken =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);
    ESCDerivedFromDataToken escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterToken =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDataToken, contentionFactor);
    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterToken =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, contentionFactor);

    FLE2InsertUpdatePayloadV2 iupayload;

    iupayload.setEdcDerivedToken(edcDataCounterToken.toCDR());
    iupayload.setEscDerivedToken(escDataCounterToken.toCDR());
    iupayload.setServerEncryptionToken(serverEncryptToken.toCDR());
    iupayload.setServerDerivedFromDataToken(serverDerivedFromDataToken.toCDR());

    auto swEncryptedTokens =
        EncryptedStateCollectionTokensV2(escDataCounterToken).serialize(ecocToken);
    uassertStatusOK(swEncryptedTokens);
    iupayload.setEncryptedTokens(swEncryptedTokens.getValue());

    auto swCipherText = KeyIdAndValue::serialize(userKey, value);
    uassertStatusOK(swCipherText);
    iupayload.setValue(swCipherText.getValue());
    iupayload.setType(element.type());
    iupayload.setIndexKeyId(indexKey.keyId);
    iupayload.setContentionFactor(contentionFactor);

    return iupayload;
}

std::unique_ptr<Edges> getEdges(FLE2RangeInsertSpec spec, int sparsity) {
    auto element = spec.getValue().getElement();
    auto minBound = spec.getMinBound().map([](IDLAnyType m) { return m.getElement(); });
    auto maxBound = spec.getMaxBound().map([](IDLAnyType m) { return m.getElement(); });

    switch (element.type()) {
        case BSONType::NumberInt:
            uassert(6775501,
                    "min bound must be integer",
                    !minBound.has_value() || minBound->type() == BSONType::NumberInt);
            uassert(6775502,
                    "max bound must be integer",
                    !maxBound.has_value() || maxBound->type() == BSONType::NumberInt);
            return getEdgesInt32(element.Int(),
                                 minBound.map([](BSONElement m) { return m.Int(); }),
                                 maxBound.map([](BSONElement m) { return m.Int(); }),
                                 sparsity);

        case BSONType::NumberLong:
            uassert(6775503,
                    "min bound must be long int",
                    !minBound.has_value() || minBound->type() == BSONType::NumberLong);
            uassert(6775504,
                    "max bound must be long int",
                    !maxBound.has_value() || maxBound->type() == BSONType::NumberLong);
            return getEdgesInt64(element.Long(),
                                 minBound.map([](BSONElement m) { return int64_t(m.Long()); }),
                                 maxBound.map([](BSONElement m) { return int64_t(m.Long()); }),
                                 sparsity);

        case BSONType::Date:
            uassert(6775505,
                    "min bound must be date",
                    !minBound.has_value() || minBound->type() == BSONType::Date);
            uassert(6775506,
                    "max bound must be date",
                    !maxBound.has_value() || maxBound->type() == BSONType::Date);
            return getEdgesInt64(element.Date().asInt64(),
                                 minBound.map([](BSONElement m) { return m.Date().asInt64(); }),
                                 maxBound.map([](BSONElement m) { return m.Date().asInt64(); }),
                                 sparsity);

        case BSONType::NumberDouble:
            uassert(6775507,
                    "min bound must be double",
                    !minBound.has_value() || minBound->type() == BSONType::NumberDouble);
            uassert(6775508,
                    "max bound must be double",
                    !maxBound.has_value() || maxBound->type() == BSONType::NumberDouble);
            return getEdgesDouble(
                element.Double(),
                minBound.map([](BSONElement m) { return m.Double(); }),
                maxBound.map([](BSONElement m) { return m.Double(); }),
                spec.getPrecision().map([](std::int32_t m) { return static_cast<uint32_t>(m); }),
                sparsity);

        case BSONType::NumberDecimal:
            uassert(6775509,
                    "min bound must be decimal",
                    !minBound.has_value() || minBound->type() == BSONType::NumberDecimal);
            uassert(6775510,
                    "max bound must be decimal",
                    !maxBound.has_value() || maxBound->type() == BSONType::NumberDecimal);
            return getEdgesDecimal128(
                element.numberDecimal(),
                minBound.map([](BSONElement m) { return m.numberDecimal(); }),
                maxBound.map([](BSONElement m) { return m.numberDecimal(); }),
                spec.getPrecision().map([](std::int32_t m) { return static_cast<uint32_t>(m); }),
                sparsity);

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
        ConstDataRange cdr(edge.rawData(), edge.size());

        EDCDerivedFromDataToken edcDatakey =
            FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, cdr);
        ESCDerivedFromDataToken escDatakey =
            FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, cdr);

        EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
            FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
                generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDatakey,
                                                                        contentionFactor);
        ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
            FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
                generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey,
                                                                        contentionFactor);
        ServerDerivedFromDataToken serverDatakey =
            FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(
                serverDerivationToken, cdr);

        EdgeTokenSetV2 ets;

        ets.setEdcDerivedToken(edcDataCounterkey.toCDR());
        ets.setEscDerivedToken(escDataCounterkey.toCDR());
        ets.setServerDerivedFromDataToken(serverDatakey.toCDR());

        auto swEncryptedTokens =
            EncryptedStateCollectionTokensV2(escDataCounterkey).serialize(ecocToken);
        uassertStatusOK(swEncryptedTokens);
        ets.setEncryptedTokens(swEncryptedTokens.getValue());

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

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey.key);
    auto serverEncryptToken =
        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(indexKey.key);
    auto serverDerivationToken =
        FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(indexKey.key);

    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);
    auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collectionToken);
    auto serverDerivedFromDataToken =
        FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(serverDerivationToken,
                                                                             value);

    EDCDerivedFromDataToken edcDatakey =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);
    ESCDerivedFromDataToken escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);

    EDCDerivedFromDataTokenAndContentionFactorToken edcDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateEDCDerivedFromDataTokenAndContentionFactorToken(edcDatakey, contentionFactor);
    ESCDerivedFromDataTokenAndContentionFactorToken escDataCounterkey =
        FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
            generateESCDerivedFromDataTokenAndContentionFactorToken(escDatakey, contentionFactor);

    FLE2InsertUpdatePayloadV2 iupayload;

    iupayload.setEdcDerivedToken(edcDataCounterkey.toCDR());
    iupayload.setEscDerivedToken(escDataCounterkey.toCDR());
    iupayload.setServerEncryptionToken(serverEncryptToken.toCDR());
    iupayload.setServerDerivedFromDataToken(serverDerivedFromDataToken.toCDR());

    auto swEncryptedTokens =
        EncryptedStateCollectionTokensV2(escDataCounterkey).serialize(ecocToken);
    uassertStatusOK(swEncryptedTokens);
    iupayload.setEncryptedTokens(swEncryptedTokens.getValue());

    auto swCipherText = KeyIdAndValue::serialize(userKey, value);
    uassertStatusOK(swCipherText);
    iupayload.setValue(swCipherText.getValue());
    iupayload.setType(element.type());
    iupayload.setIndexKeyId(indexKey.keyId);
    iupayload.setContentionFactor(contentionFactor);

    auto edgeTokenSet = getEdgeTokenSet(
        spec, sparsity, contentionFactor, edcToken, escToken, ecocToken, serverDerivationToken);

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
                    FLE2RangeInsertSpec::parse(ctx, ep.getValue().getElement().Obj());

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
                    FLE2RangeFindSpec::parse(ctx, ep.getValue().getElement().Obj());

                auto findPayload = [&]() {
                    if (rangeFindSpec.getEdgesInfo().has_value()) {
                        auto edges = getMinCover(rangeFindSpec, ep.getSparsity().value());

                        return FLEClientCrypto::serializeFindRangePayloadV2(
                            indexKey,
                            userKey,
                            std::move(edges),
                            ep.getMaxContentionCounter(),
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
        } else if (ep.getAlgorithm() == Fle2AlgorithmInt::kUnindexed) {
            uassert(6379102,
                    str::stream() << "Type '" << typeName(el.type())
                                  << "' is not a valid type for Queryable Encryption",
                    isFLE2UnindexedSupportedType(el.type()));

            auto payload = FLE2UnindexedEncryptedValueV2::serialize(userKey, el);
            builder->appendBinData(
                fieldNameToSerialize, payload.size(), BinDataType::Encrypt, payload.data());

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
    payloadInfo.fieldPathName = fieldPath.toString();

    uassert(7291901,
            "Encountered a Queryable Encryption insert/update payload type that is no "
            "longer supported",
            type == EncryptedBinDataType::kFLE2InsertUpdatePayloadV2);
    auto iupayload = EDCClientPayload::parseInsertUpdatePayloadV2(subCdr);
    payloadInfo.payload = std::move(iupayload);

    auto bsonType = static_cast<BSONType>(payloadInfo.payload.getType());

    if (payloadInfo.isRangePayload()) {
        uassert(6775305,
                str::stream() << "Type '" << typeName(bsonType)
                              << "' is not a valid type for Queryable Encryption Range",
                isValidBSONType(payloadInfo.payload.getType()) &&
                    isFLE2RangeIndexedSupportedType(bsonType));
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
            auto& v2Payload = payload->payload;

            FLE2IndexedRangeEncryptedValueV2 sp(
                v2Payload, EDCServerCollection::generateTags(*payload), payload->counts);

            uassert(7291908,
                    str::stream() << "Type '" << typeName(sp.bsonType)
                                  << "' is not a valid type for Queryable Encryption Range",
                    isFLE2RangeIndexedSupportedType(sp.bsonType));

            std::vector<ServerDerivedFromDataToken> edgeDerivedTokens;
            auto serverToken = FLETokenFromCDR<FLETokenType::ServerDataEncryptionLevel1Token>(
                v2Payload.getServerEncryptionToken());
            for (auto& ets : v2Payload.getEdgeTokenSet().value()) {
                edgeDerivedTokens.push_back(
                    FLETokenFromCDR<FLETokenType::ServerDerivedFromDataToken>(
                        ets.getServerDerivedFromDataToken()));
            }

            auto swEncrypted = sp.serialize(serverToken, edgeDerivedTokens);
            uassertStatusOK(swEncrypted);
            toEncryptedBinData(fieldPath,
                               EncryptedBinDataType::kFLE2RangeIndexedValueV2,
                               ConstDataRange(swEncrypted.getValue()),
                               builder);

            for (auto& mblock : sp.metadataBlocks) {
                pTags->push_back({mblock.tag});
            }

        } else {
            dassert(payload->counts.size() == 1);

            auto tag = EDCServerCollection::generateTag(*payload);
            auto& v2Payload = payload->payload;
            FLE2IndexedEqualityEncryptedValueV2 sp(v2Payload, tag, payload->counts[0]);

            uassert(7291906,
                    str::stream() << "Type '" << typeName(sp.bsonType)
                                  << "' is not a valid type for Queryable Encryption Equality",
                    isFLE2EqualityIndexedSupportedType(sp.bsonType));

            auto swEncrypted =
                sp.serialize(FLETokenFromCDR<FLETokenType::ServerDataEncryptionLevel1Token>(
                                 v2Payload.getServerEncryptionToken()),
                             FLETokenFromCDR<FLETokenType::ServerDerivedFromDataToken>(
                                 v2Payload.getServerDerivedFromDataToken()));
            uassertStatusOK(swEncrypted);
            toEncryptedBinData(fieldPath,
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
            docLength < static_cast<size_t>(std::numeric_limits<int32_t>::max()));

    builder.appendNum(static_cast<uint32_t>(docLength));
    builder.appendChar(static_cast<uint8_t>(type));
    builder.appendStr(valueString, true);
    builder.appendBuf(cdr.data(), cdr.length());
    builder.appendChar('\0');

    ConstDataRangeCursor cdc = ConstDataRangeCursor(ConstDataRange(builder.buf(), builder.len()));
    BSONObj elemWrapped = cdc.readAndAdvance<Validated<BSONObj>>();
    return elemWrapped.getOwned();
}

void collectIndexedFields(std::vector<EDCIndexedFields>* pFields,
                          ConstDataRange cdr,
                          StringData fieldPath) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);

    if (encryptedTypeBinding == EncryptedBinDataType::kFLE2EqualityIndexedValueV2 ||
        encryptedTypeBinding == EncryptedBinDataType::kFLE2RangeIndexedValueV2) {
        pFields->push_back({cdr, fieldPath.toString()});
    }
}

void collectFieldValidationInfo(stdx::unordered_map<std::string, ConstDataRange>* pFields,
                                ConstDataRange cdr,
                                StringData fieldPath) {
    pFields->insert({fieldPath.toString(), cdr});
}

stdx::unordered_map<std::string, EncryptedField> toFieldMap(const EncryptedFieldConfig& efc) {
    stdx::unordered_map<std::string, EncryptedField> fields;
    for (const auto& field : efc.getFields()) {
        fields.insert({field.getPath().toString(), field});
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
        total += 1 +
            (sp.payload.getEdgeTokenSet().has_value() ? sp.payload.getEdgeTokenSet().get().size()
                                                      : 0);
    }
    return total;
}

/**
 * Return the first bit set in a integer. 1 indexed.
 */
template <typename T>
int getFirstBitSet(T v) {
    return 64 - countLeadingZeros64(v);
}

template <>
int getFirstBitSet<boost::multiprecision::uint128_t>(const boost::multiprecision::uint128_t v) {
    return boost::multiprecision::msb(v) + 1;
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

boost::multiprecision::int128_t exp10(int x) {
    return pow(k10, x);
}

boost::multiprecision::uint128_t exp10ui128(int x) {
    return pow(k10ui, x);
}

double exp10Double(int x) {
    return pow(10, x);
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

    return crypt;
}

BSONObj runStateMachineForDecryption(mongocrypt_ctx_t* ctx, FLEKeyVault* keyVault) {
    mongocrypt_ctx_state_t state;
    bool done = false;
    BSONObj result;

    MongoCryptStatus status = MongoCryptStatus::create();

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

FLEEdgeCountInfo getEdgeCountInfo(const FLEStateCollectionReader& reader,
                                  ConstDataRange tag,
                                  FLETagQueryInterface::TagQueryType type,
                                  const boost::optional<PrfBlock>& edc) {

    uint64_t count;

    auto escToken = EDCServerPayloadInfo::getESCToken(tag);

    auto tagToken = FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escToken);
    auto valueToken = FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escToken);

    auto positions = ESCCollection::emuBinaryV2(reader, tagToken, valueToken);

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
            anchorId = ESCCollection::generateNullAnchorId(tagToken);
        } else {
            anchorId = ESCCollection::generateAnchorId(tagToken, positions.apos.value());
        }

        BSONObj anchorDoc = reader.getById(anchorId);
        uassert(7291903, "ESC anchor document not found", !anchorDoc.isEmpty());

        auto escAnchor =
            uassertStatusOK(ESCCollection::decryptAnchorDocument(valueToken, anchorDoc));
        count = escAnchor.count + 1;
    }


    if (type == FLETagQueryInterface::TagQueryType::kQuery) {
        count -= 1;
    }

    return FLEEdgeCountInfo(count, tagToken, edc.map([](const PrfBlock& prf) {
        return FLETokenFromCDR<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>(prf);
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
        case NumberInt:
            return minCoverInt32(lowerBound.safeNumberInt(),
                                 includeLowerBound,
                                 upperBound.safeNumberInt(),
                                 includeUpperBound,
                                 indexMin.Int(),
                                 indexMax.Int(),
                                 sparsity);
        case NumberLong:
            return minCoverInt64(lowerBound.safeNumberLong(),
                                 includeLowerBound,
                                 upperBound.safeNumberLong(),
                                 includeUpperBound,
                                 indexMin.Long(),
                                 indexMax.Long(),
                                 sparsity);
        case Date:
            return minCoverInt64(lowerBound.Date().asInt64(),
                                 includeLowerBound,
                                 upperBound.Date().asInt64(),
                                 includeUpperBound,
                                 indexMin.Date().asInt64(),
                                 indexMax.Date().asInt64(),
                                 sparsity);
        case NumberDouble:
            return minCoverDouble(lowerBound.numberDouble(),
                                  includeLowerBound,
                                  upperBound.numberDouble(),
                                  includeUpperBound,
                                  indexMin.numberDouble(),
                                  indexMax.numberDouble(),
                                  edgesInfo.getPrecision().map(
                                      [](std::int32_t m) { return static_cast<uint32_t>(m); }),

                                  sparsity);
        case NumberDecimal:
            return minCoverDecimal128(lowerBound.numberDecimal(),
                                      includeLowerBound,
                                      upperBound.numberDecimal(),
                                      includeUpperBound,
                                      indexMin.numberDecimal(),
                                      indexMax.numberDecimal(),
                                      edgesInfo.getPrecision().map(
                                          [](std::int32_t m) { return static_cast<uint32_t>(m); }),

                                      sparsity);
        default:
            // IDL validates that no other type is permitted.
            MONGO_UNREACHABLE_TASSERT(6901302);
    }
    MONGO_UNREACHABLE_TASSERT(6901303);
}

std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedBinData(const Value& value) {
    uassert(6672416, "Expected binData with subtype Encrypt", value.getType() == BinData);

    auto binData = value.getBinData();

    uassert(6672415, "Expected binData with subtype Encrypt", binData.type == BinDataType::Encrypt);

    return fromEncryptedConstDataRange(binDataToCDR(binData));
}

boost::optional<EncryptedBinDataType> getEncryptedBinDataType(const Value& value) {
    if (value.getType() != BSONType::BinData) {
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

CollectionsLevel1Token FLELevel1TokenGenerator::generateCollectionsLevel1Token(
    FLEIndexKey indexKey) {
    return FLEUtil::prf(hmacKey(indexKey.data), kLevel1Collection);
}

ServerTokenDerivationLevel1Token FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(
    FLEIndexKey indexKey) {
    return FLEUtil::prf(hmacKey(indexKey.data), kLevel1ServerTokenDerivation);
}

ServerDataEncryptionLevel1Token FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(
    FLEIndexKey indexKey) {
    return FLEUtil::prf(hmacKey(indexKey.data), kLevelServerDataEncryption);
}


EDCToken FLECollectionTokenGenerator::generateEDCToken(CollectionsLevel1Token token) {
    return FLEUtil::prf(token.data, kEDC);
}

ESCToken FLECollectionTokenGenerator::generateESCToken(CollectionsLevel1Token token) {
    return FLEUtil::prf(token.data, kESC);
}

ECCToken FLECollectionTokenGenerator::generateECCToken(CollectionsLevel1Token token) {
    return FLEUtil::prf(token.data, kECC);
}

ECOCToken FLECollectionTokenGenerator::generateECOCToken(CollectionsLevel1Token token) {
    return FLEUtil::prf(token.data, kECOC);
}


EDCDerivedFromDataToken FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(
    EDCToken token, ConstDataRange value) {
    return FLEUtil::prf(token.data, value);
}

ESCDerivedFromDataToken FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(
    ESCToken token, ConstDataRange value) {
    return FLEUtil::prf(token.data, value);
}

ECCDerivedFromDataToken FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(
    ECCToken token, ConstDataRange value) {
    return FLEUtil::prf(token.data, value);
}

ServerDerivedFromDataToken FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(
    ServerTokenDerivationLevel1Token token, ConstDataRange value) {
    return FLEUtil::prf(token.data, value);
}

EDCDerivedFromDataTokenAndContentionFactorToken
FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
    generateEDCDerivedFromDataTokenAndContentionFactorToken(EDCDerivedFromDataToken token,
                                                            FLECounter counter) {
    return FLEUtil::prf(token.data, counter);
}

ESCDerivedFromDataTokenAndContentionFactorToken
FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
    generateESCDerivedFromDataTokenAndContentionFactorToken(ESCDerivedFromDataToken token,
                                                            FLECounter counter) {
    return FLEUtil::prf(token.data, counter);
}

ECCDerivedFromDataTokenAndContentionFactorToken
FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
    generateECCDerivedFromDataTokenAndContentionFactorToken(ECCDerivedFromDataToken token,
                                                            FLECounter counter) {
    return FLEUtil::prf(token.data, counter);
}


EDCTwiceDerivedToken FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(
    EDCDerivedFromDataTokenAndContentionFactorToken token) {
    return FLEUtil::prf(token.data, kTwiceDerivedTokenFromEDC);
}

ESCTwiceDerivedTagToken FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(
    ESCDerivedFromDataTokenAndContentionFactorToken token) {
    return FLEUtil::prf(token.data, kTwiceDerivedTokenFromESCTag);
}

ESCTwiceDerivedValueToken FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(
    ESCDerivedFromDataTokenAndContentionFactorToken token) {
    return FLEUtil::prf(token.data, kTwiceDerivedTokenFromESCValue);
}

ECCTwiceDerivedTagToken FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(
    ECCDerivedFromDataTokenAndContentionFactorToken token) {
    return FLEUtil::prf(token.data, kTwiceDerivedTokenFromECCTag);
}

ECCTwiceDerivedValueToken FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(
    ECCDerivedFromDataTokenAndContentionFactorToken token) {
    return FLEUtil::prf(token.data, kTwiceDerivedTokenFromECCValue);
}

ServerCountAndContentionFactorEncryptionToken
FLEServerMetadataEncryptionTokenGenerator::generateServerCountAndContentionFactorEncryptionToken(
    ServerDerivedFromDataToken token) {
    return FLEUtil::prf(token.data, kServerCountAndContentionFactorEncryption);
}

ServerZerosEncryptionToken
FLEServerMetadataEncryptionTokenGenerator::generateServerZerosEncryptionToken(
    ServerDerivedFromDataToken token) {
    return FLEUtil::prf(token.data, kServerZerosEncryption);
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

StatusWith<EncryptedStateCollectionTokensV2> EncryptedStateCollectionTokensV2::decryptAndParse(
    ECOCToken token, ConstDataRange cdr) {

    auto swVec = FLEUtil::decryptData(token.toCDR(), cdr);
    if (!swVec.isOK()) {
        return swVec.getStatus();
    }

    auto& data = swVec.getValue();
    ConstDataRangeCursor cdrc(data);

    auto swToken = cdrc.readAndAdvanceNoThrow<PrfBlock>();
    if (!swToken.isOK()) {
        return swToken.getStatus();
    }

    auto escToken = ESCDerivedFromDataTokenAndContentionFactorToken(swToken.getValue());
    return EncryptedStateCollectionTokensV2(std::move(escToken));
}

StatusWith<std::vector<uint8_t>> EncryptedStateCollectionTokensV2::serialize(ECOCToken token) {
    return encryptData(token.toCDR(), esc.toCDR());
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

BSONObj FLEClientCrypto::generateCompactionTokens(const EncryptedFieldConfig& cfg,
                                                  FLEKeyVault* keyVault) {
    BSONObjBuilder tokensBuilder;
    auto& fields = cfg.getFields();
    for (const auto& field : fields) {
        auto indexKey = keyVault->getIndexKeyById(field.getKeyId());
        auto collToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey.key);
        auto ecocToken = FLECollectionTokenGenerator::generateECOCToken(collToken);
        auto tokenCdr = ecocToken.toCDR();
        tokensBuilder.appendBinData(
            field.getPath(), tokenCdr.length(), BinDataType::BinDataGeneral, tokenCdr.data());
    }
    return tokensBuilder.obj();
}

BSONObj FLEClientCrypto::decryptDocument(BSONObj& doc, FLEKeyVault* keyVault) {
    // TODO: SERVER-73851 replace with commented code once mongocrypt supports v2 decryption
    // auto crypt = createMongoCrypt();

    // SymmetricKey& key = keyVault->getKMSLocalKey();
    // auto binary = MongoCryptBinary::createFromCDR(ConstDataRange(key.getKey(),
    // key.getKeySize())); uassert(7132217,
    //         "mongocrypt_setopt_kms_provider_local failed",
    //         mongocrypt_setopt_kms_provider_local(crypt.get(), binary));

    // uassert(7132218, "mongocrypt_init failed", mongocrypt_init(crypt.get()));

    // UniqueMongoCryptCtx ctx(mongocrypt_ctx_new(crypt.get()));
    // auto input = MongoCryptBinary::createFromBSONObj(doc);
    // mongocrypt_ctx_decrypt_init(ctx.get(), input);
    // BSONObj obj = runStateMachineForDecryption(ctx.get(), keyVault);

    // return obj;

    BSONObjBuilder builder;

    auto obj = transformBSON(
        doc, [keyVault](ConstDataRange cdr, BSONObjBuilder* builder, StringData fieldPath) {
            auto [encryptedType, subCdr] = fromEncryptedConstDataRange(cdr);
            if (encryptedType == EncryptedBinDataType::kFLE2EqualityIndexedValueV2 ||
                encryptedType == EncryptedBinDataType::kFLE2RangeIndexedValueV2) {
                std::vector<uint8_t> userCipherText;
                BSONType type;
                if (encryptedType == EncryptedBinDataType::kFLE2EqualityIndexedValueV2) {
                    auto indexKeyId =
                        uassertStatusOK(FLE2IndexedEqualityEncryptedValueV2::readKeyId(subCdr));
                    auto indexKey = keyVault->getIndexKeyById(indexKeyId);
                    auto serverToken =
                        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(
                            indexKey.key);
                    userCipherText = uassertStatusOK(
                        FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptCiphertext(serverToken,
                                                                                       subCdr));
                    type =
                        uassertStatusOK(FLE2IndexedEqualityEncryptedValueV2::readBsonType(subCdr));
                } else {
                    auto indexKeyId =
                        uassertStatusOK(FLE2IndexedRangeEncryptedValueV2::readKeyId(subCdr));
                    auto indexKey = keyVault->getIndexKeyById(indexKeyId);
                    auto serverToken =
                        FLELevel1TokenGenerator::generateServerDataEncryptionLevel1Token(
                            indexKey.key);
                    userCipherText =
                        uassertStatusOK(FLE2IndexedRangeEncryptedValueV2::parseAndDecryptCiphertext(
                            serverToken, subCdr));
                    type = uassertStatusOK(FLE2IndexedRangeEncryptedValueV2::readBsonType(subCdr));
                }

                auto userKeyId = uassertStatusOK(KeyIdAndValue::readKeyId(userCipherText));
                auto userKey = keyVault->getUserKeyById(userKeyId);
                auto userData =
                    uassertStatusOK(KeyIdAndValue::decrypt(userKey.key, userCipherText));
                BSONObj obj = toBSON(type, userData);
                builder->appendAs(obj.firstElement(), fieldPath);
            } else if (encryptedType == EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2) {
                auto [type, userData] = FLE2UnindexedEncryptedValueV2::deserialize(keyVault, cdr);
                BSONObj obj = toBSON(type, userData);
                builder->appendAs(obj.firstElement(), fieldPath);
            } else {
                builder->appendBinData(fieldPath, cdr.length(), BinDataType::Encrypt, cdr.data());
            }
        });
    builder.appendElements(obj);
    return builder.obj();
}

void FLEClientCrypto::validateTagsArray(const BSONObj& doc) {
    BSONElement safeContent = doc[kSafeContent];

    uassert(6371506,
            str::stream() << "Found indexed encrypted fields but could not find " << kSafeContent,
            !safeContent.eoo());

    uassert(
        6371507, str::stream() << kSafeContent << " must be an array", safeContent.type() == Array);
}

void FLEClientCrypto::validateDocument(const BSONObj& doc,
                                       const EncryptedFieldConfig& efc,
                                       FLEKeyVault* keyVault) {
    stdx::unordered_map<std::string, ConstDataRange> validateFields;

    visitEncryptedBSON(doc, [&validateFields](ConstDataRange cdr, StringData fieldPath) {
        collectFieldValidationInfo(&validateFields, cdr, fieldPath);
    });

    auto configMap = toFieldMap(efc);

    stdx::unordered_map<PrfBlock, std::string> tags;

    // Ensure all encrypted fields are in EncryptedFieldConfig
    // It is ok for fields to be in EncryptedFieldConfig but not present
    for (const auto& field : validateFields) {
        auto configField = configMap.find(field.first);
        uassert(6371508,
                str::stream() << "Field '" << field.first
                              << "' is encrypted by not marked as an encryptedField",
                configField != configMap.end());

        auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(field.second);

        if (configField->second.getQueries().has_value()) {
            if (hasQueryType(configField->second, QueryTypeEnum::Equality)) {
                uassert(7293205,
                        str::stream() << "Field '" << field.first
                                      << "' is marked as equality but not indexed",
                        encryptedTypeBinding == EncryptedBinDataType::kFLE2EqualityIndexedValueV2);

                auto swTag = FLE2IndexedEqualityEncryptedValueV2::parseMetadataBlockTag(subCdr);
                uassertStatusOK(swTag.getStatus());
                tags.insert({swTag.getValue(), field.first});
            } else if (hasQueryType(configField->second, QueryTypeEnum::RangePreview)) {
                uassert(7293206,
                        str::stream()
                            << "Field '" << field.first << "' is marked as range but not indexed",
                        encryptedTypeBinding == EncryptedBinDataType::kFLE2RangeIndexedValueV2);

                auto swTags = FLE2IndexedRangeEncryptedValueV2::parseMetadataBlockTags(subCdr);
                uassertStatusOK(swTags.getStatus());
                for (const auto& tag : swTags.getValue()) {
                    tags.insert({tag, field.first});
                }
            }
        } else {
            uassert(7293207,
                    str::stream() << "Field '" << field.first << "' must be marked unindexed",
                    encryptedTypeBinding == EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2);
        }
    }

    BSONElement safeContent = doc[kSafeContent];

    // If there are no tags and no safeContent, then this document is not Queryable Encryption and
    // is therefore fine
    if (tags.size() == 0 && safeContent.eoo()) {
        return;
    }

    validateTagsArray(doc);

    size_t count = 0;
    for (const auto& element : safeContent.Obj()) {
        uassert(6371515,
                str::stream() << "Field'" << element.fieldNameStringData()
                              << "' must be a bindata and general subtype",
                element.isBinData(BinDataType::BinDataGeneral));

        auto vec = element._binDataVector();
        auto block = PrfBlockfromCDR(vec);

        uassert(6371510,
                str::stream() << "Missing tag for encrypted indexed field '"
                              << element.fieldNameStringData() << "'",
                tags.count(block) == 1);

        ++count;
    }

    uassert(6371516,
            str::stream() << "Mismatch in expected count of tags, Expected: '" << tags.size()
                          << "', Actual: '" << count << "'",
            count == tags.size());
}

PrfBlock ESCCollection::generateId(ESCTwiceDerivedTagToken tagToken,
                                   boost::optional<uint64_t> index) {
    if (index.has_value()) {
        return prf(tagToken.data, kESCNonNullId, index.value());
    } else {
        return prf(tagToken.data, kESCNullId, 0);
    }
}

PrfBlock ESCCollection::generateNonAnchorId(const ESCTwiceDerivedTagToken& tagToken,
                                            uint64_t cpos) {
    return FLEUtil::prf(tagToken.data, cpos);
}

PrfBlock ESCCollection::generateAnchorId(const ESCTwiceDerivedTagToken& tagToken, uint64_t apos) {
    return prf(tagToken.data, kESCAnchorId, apos);
}

PrfBlock ESCCollection::generateNullAnchorId(const ESCTwiceDerivedTagToken& tagToken) {
    return ESCCollection::generateAnchorId(tagToken, kESCNullAnchorPosition);
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

BSONObj ESCCollection::generateCompactionPlaceholderDocument(ESCTwiceDerivedTagToken tagToken,
                                                             ESCTwiceDerivedValueToken valueToken,
                                                             uint64_t index,
                                                             uint64_t count) {
    auto block = ESCCollection::generateId(tagToken, index);

    auto swCipherText = packAndEncrypt(std::tie(kESCompactionRecordValue, count), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);

    return builder.obj();
}

BSONObj ESCCollection::generateNonAnchorDocument(const ESCTwiceDerivedTagToken& tagToken,
                                                 uint64_t cpos) {
    auto block = ESCCollection::generateNonAnchorId(tagToken, cpos);
    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    return builder.obj();
}

BSONObj ESCCollection::generateAnchorDocument(const ESCTwiceDerivedTagToken& tagToken,
                                              const ESCTwiceDerivedValueToken& valueToken,
                                              uint64_t apos,
                                              uint64_t cpos) {
    auto block = ESCCollection::generateAnchorId(tagToken, apos);

    auto swCipherText = packAndEncrypt(std::tie(kESCNonNullAnchorValuePrefix, cpos), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
    return builder.obj();
}

BSONObj ESCCollection::generateNullAnchorDocument(const ESCTwiceDerivedTagToken& tagToken,
                                                  const ESCTwiceDerivedValueToken& valueToken,
                                                  uint64_t apos,
                                                  uint64_t cpos) {
    auto block = ESCCollection::generateNullAnchorId(tagToken);

    auto swCipherText = packAndEncrypt(std::tie(apos, cpos), valueToken);
    uassertStatusOK(swCipherText);

    BSONObjBuilder builder;
    toBinData(kId, block, &builder);
    toBinData(kValue, swCipherText.getValue(), &builder);
    return builder.obj();
}

StatusWith<ESCNullDocument> ESCCollection::decryptNullDocument(ESCTwiceDerivedValueToken valueToken,
                                                               BSONObj& doc) {
    return ESCCollection::decryptNullDocument(valueToken, std::move(doc));
}

StatusWith<ESCNullDocument> ESCCollection::decryptNullDocument(ESCTwiceDerivedValueToken valueToken,
                                                               BSONObj&& doc) {
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
    return ESCCollection::decryptDocument(valueToken, std::move(doc));
}

StatusWith<ESCDocument> ESCCollection::decryptDocument(ESCTwiceDerivedValueToken valueToken,
                                                       BSONObj&& doc) {
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

StatusWith<ESCDocument> ESCCollection::decryptAnchorDocument(
    const ESCTwiceDerivedValueToken& valueToken, BSONObj& doc) {
    return ESCCollection::decryptDocument(valueToken, doc);
}

boost::optional<uint64_t> ESCCollection::emuBinary(const FLEStateCollectionReader& reader,
                                                   ESCTwiceDerivedTagToken tagToken,
                                                   ESCTwiceDerivedValueToken valueToken) {
    return emuBinaryCommon<ESCCollection, ESCTwiceDerivedTagToken, ESCTwiceDerivedValueToken>(
        reader, tagToken, valueToken);
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

ESCCollection::EmuBinaryResult ESCCollection::emuBinaryV2(
    const FLEStateCollectionReader& reader,
    const ESCTwiceDerivedTagToken& tagToken,
    const ESCTwiceDerivedValueToken& valueToken) {
    auto tracker = FLEStatusSection::get().makeEmuBinaryTracker();

    auto x = ESCCollection::anchorBinaryHops(reader, tagToken, valueToken, tracker);
    auto i = ESCCollection::binaryHops(reader, tagToken, valueToken, x, tracker);
    return EmuBinaryResult{i, x};
}

boost::optional<uint64_t> ESCCollection::anchorBinaryHops(
    const FLEStateCollectionReader& reader,
    const ESCTwiceDerivedTagToken& tagToken,
    const ESCTwiceDerivedValueToken& valueToken,
    FLEStatusSection::EmuBinaryTracker& tracker) {

    uint64_t lambda;
    boost::optional<uint64_t> x;

    // 1. find null anchor
    PrfBlock nullAnchorId = ESCCollection::generateNullAnchorId(tagToken);
    BSONObj nullAnchorDoc = reader.getById(nullAnchorId);

    // 2. case: null anchor exists
    if (!nullAnchorDoc.isEmpty()) {
        auto swAnchor = ESCCollection::decryptDocument(valueToken, nullAnchorDoc);
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
    auto idGenerator = [&tagToken](uint64_t value) -> PrfBlock {
        return ESCCollection::generateAnchorId(tagToken, value);
    };

#ifdef DEBUG_ENUM_BINARY
    std::cout << fmt::format(
                     "anchor binary search start: lambda: {}, i: {}, rho: {}", lambda, x, rho)
              << std::endl;
#endif
    return binarySearchCommon(reader, rho, lambda, x, idGenerator, tracker);
}

boost::optional<uint64_t> ESCCollection::binaryHops(const FLEStateCollectionReader& reader,
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
        auto id = x.has_value() ? ESCCollection::generateAnchorId(tagToken, *x)
                                : ESCCollection::generateNullAnchorId(tagToken);
        auto doc = reader.getById(id);
        uassert(7291501, "ESC anchor document not found", !doc.isEmpty());

        auto swAnchor = ESCCollection::decryptDocument(valueToken, doc);
        uassertStatusOK(swAnchor.getStatus());
        lambda = swAnchor.getValue().count;
        i = boost::none;
    }

    // 2-4. initialize rho based on ESC
    uint64_t rho = reader.getDocumentCount();
    if (rho < 2) {
        rho = 2;
    }

    auto idGenerator = [&tagToken](uint64_t value) -> PrfBlock {
        return ESCCollection::generateNonAnchorId(tagToken, value);
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

    std::vector<std::vector<FLEEdgeCountInfo>> countInfoSets;
    countInfoSets.reserve(tokensSets.size());

    for (const auto& tokens : tokensSets) {
        std::vector<FLEEdgeCountInfo> countInfos;
        countInfos.reserve(tokens.size());

        for (const auto& token : tokens) {
            countInfos.push_back(getEdgeCountInfo(reader, token.esc, type, token.edc));
        }

        countInfoSets.emplace_back(countInfos);
    }

    return countInfoSets;
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

    auto swCipherText = packAndEncrypt(std::tie(count, count), valueToken);
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
                                                               const BSONObj& doc) {
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

    return ECCNullDocument{std::get<0>(value)};
}


FLE2FindEqualityPayloadV2 FLEClientCrypto::serializeFindPayloadV2(FLEIndexKeyAndId indexKey,
                                                                  FLEUserKeyAndId userKey,
                                                                  BSONElement element,
                                                                  uint64_t maxContentionFactor) {
    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());

    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey.key);
    auto serverToken =
        FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(indexKey.key);

    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);

    auto edcDatakey =
        FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value);
    auto escDatakey =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value);
    auto serverDataDerivedToken =
        FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(serverToken, value);

    FLE2FindEqualityPayloadV2 payload;

    payload.setEdcDerivedToken(edcDatakey.toCDR());
    payload.setEscDerivedToken(escDatakey.toCDR());
    payload.setMaxCounter(maxContentionFactor);
    payload.setServerDerivedFromDataToken(serverDataDerivedToken.toCDR());

    return payload;
}


FLE2FindRangePayloadV2 FLEClientCrypto::serializeFindRangePayloadV2(
    FLEIndexKeyAndId indexKey,
    FLEUserKeyAndId userKey,
    const std::vector<std::string>& edges,
    uint64_t maxContentionFactor,
    const FLE2RangeFindSpec& spec) {
    auto collectionToken = FLELevel1TokenGenerator::generateCollectionsLevel1Token(indexKey.key);
    auto serverToken =
        FLELevel1TokenGenerator::generateServerTokenDerivationLevel1Token(indexKey.key);

    auto edcToken = FLECollectionTokenGenerator::generateEDCToken(collectionToken);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(collectionToken);

    std::vector<EdgeFindTokenSetV2> tokens;
    for (auto const& edge : edges) {

        ConstDataRange value(edge.c_str(), edge.size());

        EdgeFindTokenSetV2 tokenSet;
        tokenSet.setEdcDerivedToken(
            FLEDerivedFromDataTokenGenerator::generateEDCDerivedFromDataToken(edcToken, value)
                .toCDR());

        tokenSet.setEscDerivedToken(
            FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, value)
                .toCDR());
        tokenSet.setServerDerivedFromDataToken(
            FLEDerivedFromDataTokenGenerator::generateServerDerivedFromDataToken(serverToken, value)
                .toCDR());
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

    return payload;
}

FLE2FindRangePayloadV2 FLEClientCrypto::serializeFindRangeStubV2(const FLE2RangeFindSpec& spec) {
    FLE2FindRangePayloadV2 payload;

    payload.setFirstOperator(spec.getFirstOperator());
    payload.setSecondOperator(spec.getSecondOperator());
    payload.setPayloadId(spec.getPayloadId());

    return payload;
}

StatusWith<ECCDocument> ECCCollection::decryptDocument(ECCTwiceDerivedValueToken valueToken,
                                                       const BSONObj& doc) {
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

boost::optional<uint64_t> ECCCollection::emuBinary(const FLEStateCollectionReader& reader,
                                                   ECCTwiceDerivedTagToken tagToken,
                                                   ECCTwiceDerivedValueToken valueToken) {
    return emuBinaryCommon<ECCCollection, ECCTwiceDerivedTagToken, ECCTwiceDerivedValueToken>(
        reader, tagToken, valueToken);
}

BSONObj ECOCCollection::generateDocument(StringData fieldName, ConstDataRange payload) {
    BSONObjBuilder builder;
    builder.append(kId, OID::gen());
    builder.append(kFieldName, fieldName);
    toBinData(kValue, payload, &builder);
    return builder.obj();
}

ECOCCompactionDocumentV2 ECOCCollection::parseAndDecryptV2(const BSONObj& doc, ECOCToken token) {
    IDLParserContext ctx("root");
    auto ecocDoc = EcocDocument::parse(ctx, doc);

    auto swTokens = EncryptedStateCollectionTokensV2::decryptAndParse(token, ecocDoc.getValue());
    uassertStatusOK(swTokens);
    auto& keys = swTokens.getValue();

    ECOCCompactionDocumentV2 ret;
    ret.fieldName = ecocDoc.getFieldName().toString();
    ret.esc = keys.esc;
    return ret;
}

FLE2TagAndEncryptedMetadataBlock::FLE2TagAndEncryptedMetadataBlock(uint64_t countParam,
                                                                   uint64_t contentionParam,
                                                                   PrfBlock tagParam)
    : count(countParam), contentionFactor(contentionParam), tag(std::move(tagParam)) {
    zeros.fill(0);
}

FLE2TagAndEncryptedMetadataBlock::FLE2TagAndEncryptedMetadataBlock(uint64_t countParam,
                                                                   uint64_t contentionParam,
                                                                   PrfBlock tagParam,
                                                                   ZerosBlob zerosParam)
    : count(countParam),
      contentionFactor(contentionParam),
      tag(std::move(tagParam)),
      zeros(std::move(zerosParam)) {}

StatusWith<std::vector<uint8_t>> FLE2TagAndEncryptedMetadataBlock::serialize(
    ServerDerivedFromDataToken token) {

    auto countEncryptionToken = FLEServerMetadataEncryptionTokenGenerator::
        generateServerCountAndContentionFactorEncryptionToken(token);
    auto zerosEncryptionToken =
        FLEServerMetadataEncryptionTokenGenerator::generateServerZerosEncryptionToken(token);

    auto swEncryptedCount = packAndEncrypt(std::tie(count, contentionFactor), countEncryptionToken);
    if (!swEncryptedCount.isOK()) {
        return swEncryptedCount;
    }

    auto swEncryptedZeros = encryptData(zerosEncryptionToken.toCDR(), ConstDataRange(zeros));
    if (!swEncryptedZeros.isOK()) {
        return swEncryptedZeros;
    }

    auto& encryptedCount = swEncryptedCount.getValue();
    auto& encryptedZeros = swEncryptedZeros.getValue();
    std::vector<uint8_t> serializedBlock(encryptedCount.size() + sizeof(PrfBlock) +
                                         encryptedZeros.size());
    size_t offset = 0;

    dassert(encryptedCount.size() == sizeof(EncryptedCountersBlob));
    dassert(encryptedZeros.size() == sizeof(EncryptedZerosBlob));
    dassert(serializedBlock.size() == sizeof(SerializedBlob));

    std::copy(encryptedCount.begin(), encryptedCount.end(), serializedBlock.begin());
    offset += encryptedCount.size();

    std::copy(tag.begin(), tag.end(), serializedBlock.begin() + offset);
    offset += sizeof(PrfBlock);

    std::copy(encryptedZeros.begin(), encryptedZeros.end(), serializedBlock.begin() + offset);

    return serializedBlock;
}

StatusWith<FLE2TagAndEncryptedMetadataBlock> FLE2TagAndEncryptedMetadataBlock::decryptAndParse(
    ServerDerivedFromDataToken token, ConstDataRange serializedBlock) {

    ConstDataRangeCursor blobCdrc(serializedBlock);

    auto swCountersBlob = blobCdrc.readAndAdvanceNoThrow<EncryptedCountersBlob>();
    if (!swCountersBlob.isOK()) {
        return swCountersBlob.getStatus();
    }

    auto swTag = blobCdrc.readAndAdvanceNoThrow<PrfBlock>();
    if (!swTag.isOK()) {
        return swTag.getStatus();
    }

    auto zerosEncryptionToken =
        FLEServerMetadataEncryptionTokenGenerator::generateServerZerosEncryptionToken(token);

    auto swZeros = decryptZerosBlob(zerosEncryptionToken, serializedBlock);

    auto countEncryptionToken = FLEServerMetadataEncryptionTokenGenerator::
        generateServerCountAndContentionFactorEncryptionToken(token);

    auto swCounters = decryptAndUnpack<uint64_t, uint64_t>(
        ConstDataRange(swCountersBlob.getValue()), countEncryptionToken);
    if (!swCounters.isOK()) {
        return swCounters.getStatus();
    }
    auto count = std::get<0>(swCounters.getValue());
    auto contentionFactor = std::get<1>(swCounters.getValue());

    return FLE2TagAndEncryptedMetadataBlock(
        count, contentionFactor, swTag.getValue(), swZeros.getValue());
}

StatusWith<PrfBlock> FLE2TagAndEncryptedMetadataBlock::parseTag(ConstDataRange serializedBlock) {
    ConstDataRangeCursor blobCdrc(serializedBlock);
    auto st = blobCdrc.advanceNoThrow(sizeof(EncryptedCountersBlob));
    if (!st.isOK()) {
        return st;
    }
    return blobCdrc.readAndAdvanceNoThrow<PrfBlock>();
}

StatusWith<FLE2TagAndEncryptedMetadataBlock::ZerosBlob>
FLE2TagAndEncryptedMetadataBlock::decryptZerosBlob(ServerZerosEncryptionToken zerosEncryptionToken,
                                                   ConstDataRange serializedBlock) {
    ConstDataRangeCursor blobCdrc(serializedBlock);

    auto st = blobCdrc.advanceNoThrow(sizeof(EncryptedCountersBlob) + sizeof(PrfBlock));
    if (!st.isOK()) {
        return st;
    }
    auto swZerosBlob = blobCdrc.readAndAdvanceNoThrow<EncryptedZerosBlob>();
    if (!swZerosBlob.isOK()) {
        return swZerosBlob.getStatus();
    }

    auto swDecryptedZeros =
        FLEUtil::decryptData(zerosEncryptionToken.toCDR(), ConstDataRange(swZerosBlob.getValue()));
    if (!swDecryptedZeros.isOK()) {
        return swDecryptedZeros.getStatus();
    }

    ConstDataRangeCursor zerosCdrc(swDecryptedZeros.getValue());
    return zerosCdrc.readAndAdvanceNoThrow<ZerosBlob>();
}

bool FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(const ZerosBlob& blob) {
    ConstDataRangeCursor cdrc(blob);
    uint64_t high = cdrc.readAndAdvance<uint64_t>();
    uint64_t low = cdrc.readAndAdvance<uint64_t>();
    return !(high | low);
}

FLE2IndexedEqualityEncryptedValueV2::FLE2IndexedEqualityEncryptedValueV2(
    const FLE2InsertUpdatePayloadV2& payload, PrfBlock tag, uint64_t counter)
    : FLE2IndexedEqualityEncryptedValueV2(
          static_cast<BSONType>(payload.getType()),
          payload.getIndexKeyId(),
          FLEUtil::vectorFromCDR(payload.getValue()),
          FLE2TagAndEncryptedMetadataBlock(
              counter, payload.getContentionFactor(), std::move(tag))) {}


FLE2IndexedEqualityEncryptedValueV2::FLE2IndexedEqualityEncryptedValueV2(
    BSONType typeParam,
    UUID indexKeyIdParam,
    std::vector<uint8_t> clientEncryptedValueParam,
    FLE2TagAndEncryptedMetadataBlock metadataBlockParam)
    : bsonType(typeParam),
      indexKeyId(std::move(indexKeyIdParam)),
      clientEncryptedValue(std::move(clientEncryptedValueParam)),
      metadataBlock(std::move(metadataBlockParam)) {
    uassert(7290803,
            "Invalid BSON Type in Queryable Encryption InsertUpdatePayloadV2",
            isValidBSONType(typeParam));
    uassert(7290804,
            "Invalid client encrypted value length in Queryable Encryption InsertUpdatePayloadV2",
            !clientEncryptedValue.empty());
}

StatusWith<UUID> FLE2IndexedEqualityEncryptedValueV2::readKeyId(
    ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return swFields.getValue().keyId;
}

StatusWith<BSONType> FLE2IndexedEqualityEncryptedValueV2::readBsonType(
    ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return swFields.getValue().bsonType;
}

StatusWith<FLE2IndexedEqualityEncryptedValueV2::ParsedFields>
FLE2IndexedEqualityEncryptedValueV2::parseAndValidateFields(ConstDataRange serializedServerValue) {
    ConstDataRangeCursor serializedServerCdrc(serializedServerValue);

    auto swIndexKeyId = serializedServerCdrc.readAndAdvanceNoThrow<UUIDBuf>();
    if (!swIndexKeyId.isOK()) {
        return swIndexKeyId.getStatus();
    }

    auto swBsonType = serializedServerCdrc.readAndAdvanceNoThrow<uint8_t>();
    if (!swBsonType.isOK()) {
        return swBsonType.getStatus();
    }

    uassert(7290801,
            "Invalid BSON Type in Queryable Encryption IndexedEqualityEncryptedValueV2",
            isValidBSONType(swBsonType.getValue()));

    auto type = static_cast<BSONType>(swBsonType.getValue());

    // the remaining length must fit one serialized FLE2TagAndEncryptedMetadataBlock
    uassert(7290802,
            "Invalid length of Queryable Encryption IndexedEqualityEncryptedValueV2",
            serializedServerCdrc.length() >=
                sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));
    auto encryptedDataSize =
        serializedServerCdrc.length() - sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob);

    ConstDataRange encryptedDataCdrc(serializedServerCdrc.data(), encryptedDataSize);
    serializedServerCdrc.advance(encryptedDataSize);
    ConstDataRange metadataBlockCdrc(serializedServerCdrc.data(),
                                     sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));

    return {{UUID::fromCDR(swIndexKeyId.getValue()), type, encryptedDataCdrc, metadataBlockCdrc}};
}

StatusWith<std::vector<uint8_t>> FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptCiphertext(
    ServerDataEncryptionLevel1Token serverEncryptionToken, ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return FLEUtil::decryptData(serverEncryptionToken.toCDR(), swFields.getValue().ciphertext);
}

StatusWith<FLE2TagAndEncryptedMetadataBlock>
FLE2IndexedEqualityEncryptedValueV2::parseAndDecryptMetadataBlock(
    ServerDerivedFromDataToken serverDataDerivedToken, ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return FLE2TagAndEncryptedMetadataBlock::decryptAndParse(serverDataDerivedToken,
                                                             swFields.getValue().metadataBlock);
}

StatusWith<PrfBlock> FLE2IndexedEqualityEncryptedValueV2::parseMetadataBlockTag(
    ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return FLE2TagAndEncryptedMetadataBlock::parseTag(swFields.getValue().metadataBlock);
}

StatusWith<std::vector<uint8_t>> FLE2IndexedEqualityEncryptedValueV2::serialize(
    ServerDataEncryptionLevel1Token serverEncryptionToken,
    ServerDerivedFromDataToken serverDataDerivedToken) {
    auto swEncryptedData =
        encryptData(serverEncryptionToken.toCDR(), ConstDataRange(clientEncryptedValue));
    if (!swEncryptedData.isOK()) {
        return swEncryptedData;
    }

    auto swSerializedMetadata = metadataBlock.serialize(serverDataDerivedToken);
    if (!swSerializedMetadata.isOK()) {
        return swSerializedMetadata;
    }

    auto cdrKeyId = indexKeyId.toCDR();
    auto& serverEncryptedValue = swEncryptedData.getValue();
    auto& serializedMetadataBlock = swSerializedMetadata.getValue();

    std::vector<uint8_t> serializedServerValue(cdrKeyId.length() + 1 + serverEncryptedValue.size() +
                                               serializedMetadataBlock.size());
    size_t offset = 0;

    std::copy(cdrKeyId.data(), cdrKeyId.data() + cdrKeyId.length(), serializedServerValue.begin());
    offset += cdrKeyId.length();

    uint8_t bsonTypeByte = bsonType;
    std::copy(&bsonTypeByte, (&bsonTypeByte) + 1, serializedServerValue.begin() + offset);
    offset++;

    std::copy(serverEncryptedValue.begin(),
              serverEncryptedValue.end(),
              serializedServerValue.begin() + offset);
    offset += serverEncryptedValue.size();

    std::copy(serializedMetadataBlock.begin(),
              serializedMetadataBlock.end(),
              serializedServerValue.begin() + offset);
    return serializedServerValue;
}

template <class UnindexedValue>
std::vector<uint8_t> serializeUnindexedEncryptedValue(const FLEUserKeyAndId& userKey,
                                                      const BSONElement& element) {
    BSONType bsonType = element.type();
    uassert(6379107,
            "Invalid BSON data type for Queryable Encryption",
            isFLE2UnindexedSupportedType(bsonType));

    auto value = ConstDataRange(element.value(), element.value() + element.valuesize());
    auto cdrKeyId = userKey.keyId.toCDR();
    auto cdrKey = userKey.key.toCDR();

    auto cipherTextSize = crypto::fle2AeadCipherOutputLength(value.length(), UnindexedValue::mode);
    std::vector<uint8_t> buf(UnindexedValue::assocDataSize + cipherTextSize);
    DataRangeCursor adc(buf);
    adc.writeAndAdvance(static_cast<uint8_t>(UnindexedValue::fleType));
    adc.writeAndAdvance(cdrKeyId);
    adc.writeAndAdvance(static_cast<uint8_t>(bsonType));

    ConstDataRange assocData(buf.data(), UnindexedValue::assocDataSize);
    auto cipherText = uassertStatusOK(
        encryptDataWithAssociatedData(cdrKey, assocData, value, UnindexedValue::mode));
    uassert(6379106, "Cipher text size mismatch", cipherTextSize == cipherText.size());
    adc.writeAndAdvance(ConstDataRange(cipherText));

    return buf;
}

std::vector<uint8_t> FLE2UnindexedEncryptedValueV2::serialize(const FLEUserKeyAndId& userKey,
                                                              const BSONElement& element) {
    return serializeUnindexedEncryptedValue<FLE2UnindexedEncryptedValueV2>(userKey, element);
}

template <class UnindexedValue>
std::pair<BSONType, std::vector<uint8_t>> deserializeUnindexedEncryptedValue(FLEKeyVault* keyVault,
                                                                             ConstDataRange blob) {
    auto [assocDataCdr, cipherTextCdr] = blob.split(UnindexedValue::assocDataSize);
    ConstDataRangeCursor adc(assocDataCdr);

    uint8_t marker = adc.readAndAdvance<uint8_t>();
    uassert(6379110, "Invalid data type", static_cast<uint8_t>(UnindexedValue::fleType) == marker);

    UUID keyId = UUID::fromCDR(adc.readAndAdvance<UUIDBuf>());
    auto userKey = keyVault->getUserKeyById(keyId);

    BSONType bsonType = static_cast<BSONType>(adc.read<uint8_t>());
    uassert(6379111,
            "Invalid BSON data type for Queryable Encryption",
            isFLE2UnindexedSupportedType(bsonType));

    auto data = uassertStatusOK(decryptDataWithAssociatedData(
        userKey.key.toCDR(), assocDataCdr, cipherTextCdr, UnindexedValue::mode));
    return {bsonType, data};
}

std::pair<BSONType, std::vector<uint8_t>> FLE2UnindexedEncryptedValueV2::deserialize(
    FLEKeyVault* keyVault, ConstDataRange blob) {
    return deserializeUnindexedEncryptedValue<FLE2UnindexedEncryptedValueV2>(keyVault, blob);
}


FLE2IndexedRangeEncryptedValueV2::FLE2IndexedRangeEncryptedValueV2(
    const FLE2InsertUpdatePayloadV2& payload,
    std::vector<PrfBlock> tags,
    const std::vector<uint64_t>& counters)
    : bsonType(static_cast<BSONType>(payload.getType())),
      indexKeyId(payload.getIndexKeyId()),
      clientEncryptedValue(FLEUtil::vectorFromCDR(payload.getValue())) {

    uassert(7290900,
            "Tags and counters parameters must be non-zero and of the same length",
            tags.size() == counters.size() && tags.size() > 0);
    uassert(7290901,
            "Invalid BSON Type in Queryable Encryption InsertUpdatePayloadV2",
            isValidBSONType(bsonType));
    uassert(7290902,
            "Invalid client encrypted value length in Queryable Encryption InsertUpdatePayloadV2",
            !clientEncryptedValue.empty());

    metadataBlocks.reserve(tags.size());

    for (size_t i = 0; i < tags.size(); i++) {
        metadataBlocks.push_back(
            FLE2TagAndEncryptedMetadataBlock(counters[i], payload.getContentionFactor(), tags[i]));
    }
}

FLE2IndexedRangeEncryptedValueV2::FLE2IndexedRangeEncryptedValueV2(
    BSONType typeParam,
    UUID indexKeyIdParam,
    std::vector<uint8_t> clientEncryptedValueParam,
    std::vector<FLE2TagAndEncryptedMetadataBlock> metadataBlockParam)
    : bsonType(typeParam),
      indexKeyId(std::move(indexKeyIdParam)),
      clientEncryptedValue(std::move(clientEncryptedValueParam)),
      metadataBlocks(std::move(metadataBlockParam)) {

    uassert(7290903,
            "FLE2IndexedRangeEncryptedValueV2 must have a non-zero number of edges",
            metadataBlocks.size() > 0);
    uassert(7290904,
            "Invalid BSON Type in Queryable Encryption InsertUpdatePayloadV2",
            isValidBSONType(bsonType));
    uassert(7290905,
            "Invalid client encrypted value length in Queryable Encryption InsertUpdatePayloadV2",
            !clientEncryptedValue.empty());
}

StatusWith<UUID> FLE2IndexedRangeEncryptedValueV2::readKeyId(ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return swFields.getValue().keyId;
}

StatusWith<BSONType> FLE2IndexedRangeEncryptedValueV2::readBsonType(
    ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return swFields.getValue().bsonType;
}

StatusWith<FLE2IndexedRangeEncryptedValueV2::ParsedFields>
FLE2IndexedRangeEncryptedValueV2::parseAndValidateFields(ConstDataRange serializedServerValue) {
    ConstDataRangeCursor serializedServerCdrc(serializedServerValue);

    auto swIndexKeyId = serializedServerCdrc.readAndAdvanceNoThrow<UUIDBuf>();
    if (!swIndexKeyId.isOK()) {
        return swIndexKeyId.getStatus();
    }

    auto swBsonType = serializedServerCdrc.readAndAdvanceNoThrow<uint8_t>();
    if (!swBsonType.isOK()) {
        return swBsonType.getStatus();
    }

    uassert(7290906,
            "Invalid BSON Type in Queryable Encryption IndexedRangeEncryptedValueV2",
            isValidBSONType(swBsonType.getValue()));

    auto type = static_cast<BSONType>(swBsonType.getValue());

    auto swEdgeCount = serializedServerCdrc.readAndAdvanceNoThrow<uint8_t>();
    if (!swEdgeCount.isOK()) {
        return swEdgeCount.getStatus();
    }

    auto edgeCount = swEdgeCount.getValue();

    uassert(7290908,
            "Invalid length of Queryable Encryption IndexedRangeEncryptedValueV2",
            serializedServerCdrc.length() >=
                edgeCount * sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));

    auto encryptedDataSize = serializedServerCdrc.length() -
        edgeCount * sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob);

    ConstDataRange encryptedDataCdrc(serializedServerCdrc.data(), encryptedDataSize);
    serializedServerCdrc.advance(encryptedDataSize);

    std::vector<ConstDataRange> metadataBlocks;
    metadataBlocks.reserve(edgeCount);

    for (uint8_t i = 0; i < edgeCount; i++) {
        metadataBlocks.push_back(serializedServerCdrc.sliceAndAdvance(
            sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob)));
    }

    return FLE2IndexedRangeEncryptedValueV2::ParsedFields{
        UUID::fromCDR(swIndexKeyId.getValue()), type, edgeCount, encryptedDataCdrc, metadataBlocks};
}

StatusWith<std::vector<uint8_t>> FLE2IndexedRangeEncryptedValueV2::parseAndDecryptCiphertext(
    ServerDataEncryptionLevel1Token serverEncryptionToken, ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    return FLEUtil::decryptData(serverEncryptionToken.toCDR(), swFields.getValue().ciphertext);
}

StatusWith<std::vector<FLE2TagAndEncryptedMetadataBlock>>
FLE2IndexedRangeEncryptedValueV2::parseAndDecryptMetadataBlocks(
    const std::vector<ServerDerivedFromDataToken>& serverDataDerivedTokens,
    ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    auto edgeCount = swFields.getValue().edgeCount;
    uassert(7290907,
            "Invalid length of serverDataDerivedTokens parameter",
            serverDataDerivedTokens.size() == edgeCount);

    std::vector<FLE2TagAndEncryptedMetadataBlock> metadataBlocks;
    for (uint8_t i = 0; i < edgeCount; i++) {
        auto encryptedMetadataBlockCDR = swFields.getValue().metadataBlocks[i];

        auto swMetadataBlock = FLE2TagAndEncryptedMetadataBlock::decryptAndParse(
            serverDataDerivedTokens[i], encryptedMetadataBlockCDR);

        if (!swMetadataBlock.isOK()) {
            return swMetadataBlock.getStatus();
        }

        metadataBlocks.push_back(swMetadataBlock.getValue());
    }
    return metadataBlocks;
}

StatusWith<std::vector<PrfBlock>> FLE2IndexedRangeEncryptedValueV2::parseMetadataBlockTags(
    ConstDataRange serializedServerValue) {
    auto swFields = parseAndValidateFields(serializedServerValue);
    if (!swFields.isOK()) {
        return swFields.getStatus();
    }
    auto edgeCount = swFields.getValue().edgeCount;
    std::vector<PrfBlock> tags;
    tags.reserve(edgeCount);

    for (uint8_t i = 0; i < edgeCount; i++) {
        auto swTag =
            FLE2TagAndEncryptedMetadataBlock::parseTag(swFields.getValue().metadataBlocks[i]);
        if (!swTag.isOK()) {
            return swTag.getStatus();
        }
        tags.push_back(swTag.getValue());
    }
    return tags;
}

StatusWith<std::vector<uint8_t>> FLE2IndexedRangeEncryptedValueV2::serialize(
    ServerDataEncryptionLevel1Token serverEncryptionToken,
    const std::vector<ServerDerivedFromDataToken>& serverDataDerivedTokens) {

    uassert(7290909,
            "ServerDataDerivedTokens parameter should be as long as metadata blocks",
            serverDataDerivedTokens.size() == metadataBlocks.size());

    uassert(7290910,
            "Size of serverDataDerivedTokens is too large",
            serverDataDerivedTokens.size() < UINT8_MAX);

    uint8_t edgeCount = static_cast<uint8_t>(metadataBlocks.size());

    auto swEncryptedData =
        encryptData(serverEncryptionToken.toCDR(), ConstDataRange(clientEncryptedValue));
    if (!swEncryptedData.isOK()) {
        return swEncryptedData;
    }

    auto cdrKeyId = indexKeyId.toCDR();
    auto& serverEncryptedValue = swEncryptedData.getValue();


    std::vector<uint8_t> serializedServerValue(
        cdrKeyId.length() + 1 + 1 + serverEncryptedValue.size() +
        edgeCount * sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));

    size_t offset = 0;

    std::copy(cdrKeyId.data(), cdrKeyId.data() + cdrKeyId.length(), serializedServerValue.begin());
    offset += cdrKeyId.length();

    uint8_t bsonTypeByte = bsonType;
    std::copy(&bsonTypeByte, (&bsonTypeByte) + 1, serializedServerValue.begin() + offset);
    offset++;

    std::copy(&edgeCount, (&edgeCount) + 1, serializedServerValue.begin() + offset);
    offset++;

    std::copy(serverEncryptedValue.begin(),
              serverEncryptedValue.end(),
              serializedServerValue.begin() + offset);
    offset += serverEncryptedValue.size();

    for (size_t i = 0; i < metadataBlocks.size(); i++) {
        auto& metadataBlock = metadataBlocks[i];
        auto& serverDataDerivedToken = serverDataDerivedTokens[i];

        auto swSerializedMetadata = metadataBlock.serialize(serverDataDerivedToken);
        if (!swSerializedMetadata.isOK()) {
            return swSerializedMetadata.getStatus();
        }

        auto& serializedMetadata = swSerializedMetadata.getValue();

        uassert(7290911,
                "Serialized metadata is incorrect length",
                serializedMetadata.size() ==
                    sizeof(FLE2TagAndEncryptedMetadataBlock::SerializedBlob));

        std::copy(serializedMetadata.begin(),
                  serializedMetadata.end(),
                  serializedServerValue.begin() + offset);

        offset += serializedMetadata.size();
    }

    return serializedServerValue;
}


ESCDerivedFromDataTokenAndContentionFactorToken EDCServerPayloadInfo::getESCToken(
    ConstDataRange cdr) {
    return FLETokenFromCDR<FLETokenType::ESCDerivedFromDataTokenAndContentionFactorToken>(cdr);
}

void EDCServerCollection::validateEncryptedFieldInfo(BSONObj& obj,
                                                     const EncryptedFieldConfig& efc,
                                                     bool bypassDocumentValidation) {
    stdx::unordered_set<std::string> indexedFields;
    for (const auto& f : efc.getFields()) {
        if (f.getQueries().has_value()) {
            indexedFields.insert(f.getPath().toString());
        }
    }

    visitEncryptedBSON(obj, [&indexedFields](ConstDataRange cdr, StringData fieldPath) {
        auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);

        if (encryptedTypeBinding == EncryptedBinDataType::kFLE2InsertUpdatePayloadV2) {
            uassert(6373601,
                    str::stream() << "Field '" << fieldPath
                                  << "' is encrypted, but absent from schema",
                    indexedFields.contains(fieldPath.toString()));
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

PrfBlock EDCServerCollection::generateTag(EDCTwiceDerivedToken edcTwiceDerived, FLECounter count) {
    return FLEUtil::prf(edcTwiceDerived.toCDR(), count);
}

PrfBlock EDCServerCollection::generateTag(const EDCServerPayloadInfo& payload) {
    auto token = FLETokenFromCDR<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>(
        payload.payload.getEdcDerivedToken());
    auto edcTwiceDerived = FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(token);
    dassert(payload.isRangePayload() == false);
    dassert(payload.counts.size() == 1);
    return generateTag(edcTwiceDerived, payload.counts[0]);
}

std::vector<PrfBlock> EDCServerCollection::generateTags(const EDCServerPayloadInfo& rangePayload) {
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

    for (size_t i = 0; i < edgeTokenSets.size(); i++) {
        auto edcTwiceDerived = FLETwiceDerivedTokenGenerator::generateEDCTwiceDerivedToken(
            FLETokenFromCDR<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>(
                edgeTokenSets[i].getEdcDerivedToken()));
        tags.push_back(EDCServerCollection::generateTag(edcTwiceDerived, rangePayload.counts[i]));
    }
    return tags;
}

std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> EDCServerCollection::generateEDCTokens(
    EDCDerivedFromDataToken token, uint64_t maxContentionFactor) {
    std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> tokens;
    tokens.reserve(maxContentionFactor);

    for (uint64_t i = 0; i <= maxContentionFactor; ++i) {
        tokens.push_back(FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
                             generateEDCDerivedFromDataTokenAndContentionFactorToken(token, i));
    }

    return tokens;
}

std::vector<EDCDerivedFromDataTokenAndContentionFactorToken> EDCServerCollection::generateEDCTokens(
    ConstDataRange rawToken, uint64_t maxContentionFactor) {
    auto token = FLETokenFromCDR<FLETokenType::EDCDerivedFromDataToken>(rawToken);

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
                    element.type() == Object);
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
            auto swTag = FLE2IndexedEqualityEncryptedValueV2::parseMetadataBlockTag(subCdr);
            uassertStatusOK(swTag.getStatus());
            staleTags.push_back(swTag.getValue());
        } else if (encryptedTypeBinding == EncryptedBinDataType::kFLE2RangeIndexedValueV2) {
            auto swTags = FLE2IndexedRangeEncryptedValueV2::parseMetadataBlockTags(subCdr);
            uassertStatusOK(swTags.getStatus());
            auto& rangeTags = swTags.getValue();
            staleTags.insert(staleTags.end(), rangeTags.begin(), rangeTags.end());
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

    ei.setSchema(BSON(nss.toString() << encryptedFields));

    return ei.toBSON();
}

EncryptedFieldConfig EncryptionInformationHelpers::getAndValidateSchema(
    const NamespaceString& nss, const EncryptionInformation& ei) {
    BSONObj schema = ei.getSchema();

    auto element = schema.getField(nss.toString());

    uassert(6371205,
            "Expected an object for schema in EncryptionInformation",
            !element.eoo() && element.type() == Object);

    auto efc = EncryptedFieldConfig::parse(IDLParserContext("schema"), element.Obj());

    uassert(6371207, "Expected a value for escCollection", efc.getEscCollection().has_value());
    uassert(6371208, "Expected a value for ecocCollection", efc.getEcocCollection().has_value());

    return efc;
}


std::pair<EncryptedBinDataType, ConstDataRange> fromEncryptedConstDataRange(ConstDataRange cdr) {
    ConstDataRangeCursor cdrc(cdr);

    uint8_t subTypeByte = cdrc.readAndAdvance<uint8_t>();

    auto subType = EncryptedBinDataType_parse(IDLParserContext("subtype"), subTypeByte);
    return {subType, cdrc};
}

ParsedFindEqualityPayload::ParsedFindEqualityPayload(BSONElement fleFindPayload)
    : ParsedFindEqualityPayload(binDataToCDR(fleFindPayload)){};

ParsedFindEqualityPayload::ParsedFindEqualityPayload(const Value& fleFindPayload)
    : ParsedFindEqualityPayload(binDataToCDR(fleFindPayload)){};

ParsedFindEqualityPayload::ParsedFindEqualityPayload(ConstDataRange cdr) {
    auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(cdr);
    auto encryptedType = encryptedTypeBinding;

    uassert(7292600,
            str::stream() << "Unexpected encrypted payload type: "
                          << static_cast<uint32_t>(encryptedType),
            encryptedType == EncryptedBinDataType::kFLE2FindEqualityPayloadV2);

    auto payload = parseFromCDR<FLE2FindEqualityPayloadV2>(subCdr);

    escToken = FLETokenFromCDR<FLETokenType::ESCDerivedFromDataToken>(payload.getEscDerivedToken());
    edcToken = FLETokenFromCDR<FLETokenType::EDCDerivedFromDataToken>(payload.getEdcDerivedToken());
    serverDataDerivedToken = FLETokenFromCDR<FLETokenType::ServerDerivedFromDataToken>(
        payload.getServerDerivedFromDataToken());
    maxCounter = payload.getMaxCounter();
}

ParsedFindRangePayload::ParsedFindRangePayload(BSONElement fleFindPayload)
    : ParsedFindRangePayload(binDataToCDR(fleFindPayload)){};

ParsedFindRangePayload::ParsedFindRangePayload(const Value& fleFindPayload)
    : ParsedFindRangePayload(binDataToCDR(fleFindPayload)){};

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

    if (!payload.getPayload()) {
        return;
    }

    edges = std::vector<FLEFindEdgeTokenSet>();
    auto& edgesRef = edges.value();
    auto& info = payload.getPayload().value();

    for (auto const& edge : info.getEdges()) {
        auto escToken =
            FLETokenFromCDR<FLETokenType::ESCDerivedFromDataToken>(edge.getEscDerivedToken());
        auto edcToken =
            FLETokenFromCDR<FLETokenType::EDCDerivedFromDataToken>(edge.getEdcDerivedToken());
        auto serverDataDerivedToken = FLETokenFromCDR<FLETokenType::ServerDerivedFromDataToken>(
            edge.getServerDerivedFromDataToken());
        edgesRef.push_back({edcToken, escToken, serverDataDerivedToken});
    }

    maxCounter = info.getMaxCounter();
}


std::vector<CompactionToken> CompactionHelpers::parseCompactionTokens(BSONObj compactionTokens) {
    std::vector<CompactionToken> parsed;

    for (auto& elem : compactionTokens) {
        uassert(6346801,
                str::stream() << "Field '" << elem.fieldNameStringData()
                              << "' of compaction tokens must be a bindata and general subtype",
                elem.isBinData(BinDataType::BinDataGeneral));

        auto vec = elem._binDataVector();
        auto block = PrfBlockfromCDR(vec);

        parsed.push_back({elem.fieldNameStringData().toString(), ECOCToken(std::move(block))});
    }
    return parsed;
}

void CompactionHelpers::validateCompactionTokens(const EncryptedFieldConfig& efc,
                                                 BSONObj compactionTokens) {
    for (const auto& field : efc.getFields()) {
        const auto& tokenElement = compactionTokens.getField(field.getPath());
        uassert(
            6346806,
            str::stream()
                << "Compaction tokens object is missing compaction token for the encrypted path '"
                << field.getPath() << "'",
            !tokenElement.eoo());
    }
}

ConstDataRange binDataToCDR(BSONElement element) {
    uassert(6338501, "Expected binData BSON element", element.type() == BinData);

    int len;
    const char* data = element.binData(len);
    return ConstDataRange(data, data + len);
}

bool hasQueryType(const EncryptedField& field, QueryTypeEnum queryType) {
    if (!field.getQueries()) {
        return false;
    }

    return stdx::visit(OverloadedVisitor{[&](QueryTypeConfig query) {
                                             return (query.getQueryType() == queryType);
                                         },
                                         [&](std::vector<QueryTypeConfig> queries) {
                                             return std::any_of(queries.cbegin(),
                                                                queries.cend(),
                                                                [&](const QueryTypeConfig& qtc) {
                                                                    return qtc.getQueryType() ==
                                                                        queryType;
                                                                });
                                         }},
                       field.getQueries().get());
}

bool hasQueryType(const EncryptedFieldConfig& config, QueryTypeEnum queryType) {

    for (const auto& field : config.getFields()) {

        if (field.getQueries().has_value()) {
            bool hasQuery = hasQueryType(field, queryType);
            if (hasQuery) {
                return hasQuery;
            }
        }
    }

    return false;
}

/**
 * Encode a signed 32-bit integer as an unsigned 32-bit integer
 */
uint32_t encodeInt32(int32_t v) {
    if (v < 0) {

        // Signed integers have a value that there is no positive equivalent and must be handled
        // specially
        if (v == std::numeric_limits<int32_t>::min()) {
            return 0;
        }

        return (v & ~(1U << 31));
    }

    return v + (1U << 31);
}


OSTType_Int32 getTypeInfo32(int32_t value,
                            boost::optional<int32_t> min,
                            boost::optional<int32_t> max) {
    uassert(6775001,
            "Must specify both a lower and upper bound or no bounds.",
            min.has_value() == max.has_value());

    if (!min.has_value()) {
        uint32_t uv = encodeInt32(value);
        return {uv, 0, std::numeric_limits<uint32_t>::max()};
    } else {
        uassert(6775002,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());
        uassert(6775003,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());

        // Handle min int32 as a special case
        if (min.value() == std::numeric_limits<int32_t>::min()) {
            uint32_t uv = encodeInt32(value);
            return {uv, 0, encodeInt32(max.value())};
        }

        // For negative numbers, first convert them to unbiased uint32 and then subtract the min
        // value.
        if (min.value() < 0) {
            uint32_t uv = encodeInt32(value);
            uint32_t min_v = encodeInt32(min.value());
            uint32_t max_v = encodeInt32(max.value());

            uv -= min_v;
            max_v -= min_v;

            return {uv, 0, max_v};
        }

        return {static_cast<uint32_t>(value - min.value()),
                0,
                static_cast<uint32_t>(max.value() - min.value())};
    }
}

/**
 * Encode a signed 64-bit integer as an unsigned 64-bit integer
 */
uint64_t encodeInt64(int64_t v) {
    if (v < 0) {

        // Signed integers have a value that there is no positive equivalent and must be handled
        // specially
        if (v == std::numeric_limits<int64_t>::min()) {
            return 0;
        }

        return (v & ~(1ULL << 63));
    }

    return v + (1ULL << 63);
}

OSTType_Int64 getTypeInfo64(int64_t value,
                            boost::optional<int64_t> min,
                            boost::optional<int64_t> max) {
    uassert(6775004,
            "Must specify both a lower and upper bound or no bounds.",
            min.has_value() == max.has_value());

    if (!min.has_value()) {
        uint64_t uv = encodeInt64(value);
        return {uv, 0, std::numeric_limits<uint64_t>::max()};
    } else {
        uassert(6775005,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());
        uassert(6775006,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());

        // Handle min int64 as a special case
        if (min.value() == std::numeric_limits<int64_t>::min()) {
            uint64_t uv = encodeInt64(value);
            return {uv, 0, encodeInt64(max.value())};
        }

        // For negative numbers, first convert them to unbiased uin64 and then subtract the min
        // value.
        if (min.value() < 0) {
            uint64_t uv = encodeInt64(value);
            uint64_t min_v = encodeInt64(min.value());
            uint64_t max_v = encodeInt64(max.value());

            uv -= min_v;
            max_v -= min_v;

            return {uv, 0, max_v};
        }

        return {static_cast<uint64_t>(value - min.value()),
                0,
                static_cast<uint64_t>(max.value() - min.value())};
    }
}

OSTType_Double getTypeInfoDouble(double value,
                                 boost::optional<double> min,
                                 boost::optional<double> max,
                                 boost::optional<uint32_t> precision) {
    uassert(6775007,
            "Must specify both a lower bound and upper bound or no bounds.",
            min.has_value() == max.has_value());

    uassert(6775008,
            "Infinity and Nan double values are not supported.",
            !std::isinf(value) && !std::isnan(value));

    if (min.has_value()) {
        uassert(6775009,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());
        uassert(6775010,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());
    }

    // Map negative 0 to zero so sign bit is 0.
    if (std::signbit(value) && value == 0) {
        value = 0;
    }

    // When we use precision mode, we try to represent as a double value that fits in [-2^63, 2^63]
    // (i.e. is a valid int64)
    //
    // This check determines if we can represent the precision truncated value as a 64-bit integer
    // I.e. Is ((ub - lb) * 10^precision) < 64 bits.
    //
    // It is important we determine whether a range and its precision fit without looking that value
    // because the encoding for precision truncated doubles is incompatible with the encoding for
    // doubles without precision.
    //
    bool use_precision_mode = false;
    uint32_t bits_range;
    if (precision.has_value()) {

        // Subnormal representations can support up to 5x10^-324 as a number
        uassert(6966801, "Precision must be between 0 and 324 inclusive", precision.get() <= 324);

        uassert(6966803,
                "Must specify both a lower bound, upper bound and precision",
                min.has_value() == max.has_value() && max.has_value() == precision.has_value());

        double range = max.get() - min.get();

        // We can overflow if max = max double and min = min double so make sure we have finite
        // number after we do subtraction
        if (std::isfinite(range)) {

            // This creates a range which is wider then we permit by our min/max bounds check with
            // the +1 but it is as the algorithm is written in the paper.
            double rangeAndPrecision = (range + 1) * exp10Double(precision.get());

            if (std::isfinite(rangeAndPrecision)) {

                double bits_range_double = log2(rangeAndPrecision);
                bits_range = ceil(bits_range_double);

                if (bits_range < 64) {
                    use_precision_mode = true;
                }
            }
        }
    }

    if (use_precision_mode) {

        // Take a number of xxxx.ppppp and truncate it xxxx.ppp if precision = 3. We do not change
        // the digits before the decimal place.
        double v_prime = trunc(value * exp10Double(precision.get())) / exp10Double(precision.get());
        int64_t v_prime2 = (v_prime - min.get()) * exp10Double(precision.get());

        invariant(v_prime2 < std::numeric_limits<int64_t>::max() && v_prime2 >= 0);

        uint64_t ret = static_cast<uint64_t>(v_prime2);

        // Adjust maximum value to be the max bit range. This will be used by getEdges/minCover to
        // trim bits.
        uint64_t max_value = (1ULL << bits_range) - 1;
        invariant(ret <= max_value);

        return {ret, 0, max_value};
    }

    // When we translate the double into "bits", the sign bit means that the negative numbers
    // get mapped into the higher 63 bits of a 64-bit integer. We want them to map into the lower
    // 64-bits so we invert the sign bit.
    //
    // On Endianness, we support two sets of architectures
    // 1. Little Endian (ppc64le, x64, aarch64) - in these architectures, int64 and double are both
    // 64-bits and both arranged in little endian byte order.
    // 2. Big Endian (s390x) - in these architectures, int64 and double are both
    // 64-bits and both arranged in big endian byte order.
    //
    // Therefore, since the order of bytes on each platform is consistent with itself, the
    // conversion below converts a double into correct 64-bit integer that produces the same
    // behavior across plaforms.
    bool is_neg = value < 0;

    value *= -1;
    char* buf = reinterpret_cast<char*>(&value);
    uint64_t uv = DataView(buf).read<uint64_t>();

    if (is_neg) {
        dassert(uv < std::numeric_limits<uint64_t>::max());
        uv = (1ULL << 63) - uv;
    }

    return {uv, 0, std::numeric_limits<uint64_t>::max()};
}

boost::multiprecision::uint128_t toInt128FromDecimal128(Decimal128 dec) {
    // This algorithm only works because it assumes we are dealing with Decimal128 numbers that are
    // valid uint128 numbers. This means the Decimal128 has to be an integer or else the result is
    // undefined.
    invariant(dec.isFinite());
    invariant(!dec.isNegative());

    // If after rounding, the number has changed, we have a fraction, not an integer.
    invariant(dec.round() == dec);

    boost::multiprecision::uint128_t ret(dec.getCoefficientHigh());

    ret <<= 64;
    ret |= dec.getCoefficientLow();

    auto exponent = static_cast<int32_t>(dec.getBiasedExponent()) - Decimal128::kExponentBias;

    auto e1 = exp10ui128(labs(exponent));
    if (exponent < 0) {
        ret /= e1;
    } else {
        ret *= e1;
    }

    // Round-trip our new Int128 back to Decimal128 and make sure it is equal to the original
    // Decimal128 or else.
    Decimal128 roundTrip(ret.str());
    invariant(roundTrip == dec);

    return ret;
}

// For full algorithm see SERVER-68542
OSTType_Decimal128 getTypeInfoDecimal128(Decimal128 value,
                                         boost::optional<Decimal128> min,
                                         boost::optional<Decimal128> max,
                                         boost::optional<uint32_t> precision) {
    uassert(6854201,
            "Must specify both a lower bound and upper bound or no bounds.",
            min.has_value() == max.has_value());

    uassert(6854202,
            "Infinity and Nan Decimal128 values are not supported.",
            !value.isInfinite() && !value.isNaN());

    if (min.has_value()) {
        uassert(6854203,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());

        uassert(6854204,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());
    }

    // When we use precision mode, we try to represent as a decimal128 value that fits in [-2^127,
    // 2^127] (i.e. is a valid int128)
    //
    // This check determines if we can represent the precision truncated value as a 128-bit integer
    // I.e. Is ((ub - lb) * 10^precision) < 128 bits.
    //
    // It is important we determine whether a range and its precision fit without looking that value
    // because the encoding for precision truncated decimal128 is incompatible with normal
    // decimal128 values.
    bool use_precision_mode = false;
    int bits_range = 0;
    if (precision.has_value()) {
        uassert(6966804,
                "Must specify both a lower bound, upper bound and precision",
                min.has_value() == max.has_value() && max.has_value() == precision.has_value());

        uassert(6966802, "Precision must be between 0 and 6182 inclusive", precision.get() <= 6142);


        Decimal128 bounds = max.get().subtract(min.get()).add(Decimal128(1));

        if (bounds.isFinite()) {
            Decimal128 bits_range_dec = bounds.scale(precision.get()).logarithm(Decimal128(2));

            if (bits_range_dec.isFinite() && bits_range_dec < Decimal128(128)) {
                // kRoundTowardPositive is the same as C99 ceil()

                bits_range = bits_range_dec.toIntExact(Decimal128::kRoundTowardPositive);

                // bits_range is always >= 0 but coverity cannot be sure since it does not
                // understand Decimal128 math so we add a check for positive integers.
                if (bits_range >= 0 && bits_range < 128) {
                    use_precision_mode = true;
                }
            }
        }
    }

    if (use_precision_mode) {
        // Example value: 31.4159
        // Example Precision = 2

        // Shift the number up
        // Returns: 3141.9
        Decimal128 valueScaled = value.scale(precision.get());

        // Round the number down
        // Returns 3141.0
        Decimal128 valueTruncated = valueScaled.round(Decimal128::kRoundTowardZero);

        // Shift the number down
        // Returns: 31.41
        Decimal128 v_prime = valueTruncated.scale(-static_cast<int32_t>(precision.get()));

        // Adjust the number by the lower bound
        // Make it an integer by scaling the number
        //
        // Returns 3141.0
        Decimal128 v_prime2 = v_prime.subtract(min.get()).scale(precision.get());
        // Round the number down again. min may have a fractional value with more decimal places
        // than the precision (e.g. .001). Subtracting min may have resulted in v_prime2 with
        // a non-zero fraction. v_prime2 is expected to have no fractional value when
        // converting to int128.
        v_prime2 = v_prime2.round(Decimal128::kRoundTowardZero);

        invariant(v_prime2.logarithm(Decimal128(2)).isLess(Decimal128(128)));

        // Now we need to get the Decimal128 out as a 128-bit integer
        // But Decimal128 does not support conversion to Int128.
        //
        // If we think the Decimal128 fits in the range, based on the maximum value, we try to
        // convert to int64 directly.
        if (bits_range < 64) {

            // Try conversion to int64, it may fail but since it is easy we try this first.
            //
            uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;

            std::int64_t vPrimeInt264 = v_prime2.toLongExact(&signalingFlags);

            if (signalingFlags == Decimal128::SignalingFlag::kNoFlag) {
                std::uint64_t vPrimeUInt264 = static_cast<uint64_t>(vPrimeInt264);
                return {vPrimeUInt264, 0, (1ULL << bits_range) - 1};
            }
        }

        boost::multiprecision::uint128_t u_ret = toInt128FromDecimal128(v_prime2);

        boost::multiprecision::uint128_t max_dec =
            (boost::multiprecision::uint128_t(1) << bits_range) - 1;

        return {u_ret, 0, max_dec};
    }

    bool isNegative = value.isNegative();
    int32_t scale = value.getBiasedExponent() - Decimal128::kExponentBias;
    int64_t highCoefficent = value.getCoefficientHigh();
    int64_t lowCoefficient = value.getCoefficientLow();

// use int128_t where possible on gcc/clang
#ifdef __SIZEOF_INT128__
    __int128 cMax1 = 0x1ed09bead87c0;
    cMax1 <<= 64;
    cMax1 |= 0x378d8e63ffffffff;
    const boost::multiprecision::uint128_t cMax(cMax1);
    if (kDebugBuild) {
        const boost::multiprecision::uint128_t cMaxStr("9999999999999999999999999999999999");
        dassert(cMaxStr == cMax);
    }
#else
    boost::multiprecision::uint128_t cMax("9999999999999999999999999999999999");
#endif
    const int64_t eMin = -6176;

    boost::multiprecision::int128_t unscaledValue(highCoefficent);
    unscaledValue <<= 64;
    unscaledValue += lowCoefficient;

    int64_t rho = 0;
    auto stepValue = unscaledValue;

    bool flag = true;
    if (unscaledValue == 0) {
        flag = false;
    }

    while (flag != false) {
        if (stepValue > cMax) {
            flag = false;
            rho = rho - 1;
            stepValue /= k10;
        } else {
            rho = rho + 1;
            stepValue *= k10;
        }
    }

    boost::multiprecision::uint128_t mapping = 0;
    auto part2 = k1 << 127;

    if (unscaledValue == 0) {
        mapping = part2;
    } else if (rho <= scale - eMin) {
        auto part1 = stepValue + (cMax * (scale - eMin - rho));
        if (isNegative) {
            part1 = -part1;
        }

        mapping = static_cast<boost::multiprecision::uint128_t>(part1 + part2);

    } else {
        auto part1 = exp10(scale - eMin) * unscaledValue;
        if (isNegative) {
            part1 = -part1;
        }

        mapping = static_cast<boost::multiprecision::uint128_t>(part1 + part2);
    }

    return {mapping, 0, std::numeric_limits<boost::multiprecision::uint128_t>::max()};
}

EncryptedPredicateEvaluatorV2::EncryptedPredicateEvaluatorV2(
    std::vector<ServerZerosEncryptionToken> zerosTokens)
    : _zerosDecryptionTokens(std::move(zerosTokens)){};

bool EncryptedPredicateEvaluatorV2::evaluate(
    Value fieldValue,
    EncryptedBinDataType indexedValueType,
    std::function<std::vector<ConstDataRange>(ConstDataRange)> extractMetadataBlocks) const {

    if (fieldValue.getType() != BinData) {
        return false;
    }

    auto [subSubType, data] = fromEncryptedBinData(fieldValue);

    uassert(7399501, "Invalid encrypted indexed field", subSubType == indexedValueType);

    std::vector<ConstDataRange> metadataBlocks = extractMetadataBlocks(data);

    for (const auto& zeroDecryptionToken : _zerosDecryptionTokens) {
        for (auto metadataBlock : metadataBlocks) {
            auto swZerosBlob = FLE2TagAndEncryptedMetadataBlock::decryptZerosBlob(
                zeroDecryptionToken, metadataBlock);
            uassertStatusOK(swZerosBlob);
            if (FLE2TagAndEncryptedMetadataBlock::isValidZerosBlob(swZerosBlob.getValue())) {
                return true;
            }
        }
    }

    return false;
}

// Edges

Edges::Edges(std::string leaf, int sparsity) : _leaf(std::move(leaf)), _sparsity(sparsity) {
    uassert(6775101, "sparsity must be 1 or larger", _sparsity > 0);
    dassert(std::all_of(_leaf.begin(), _leaf.end(), [](char c) { return c == '1' || c == '0'; }));
}

std::vector<StringData> Edges::get() {
    static const StringData kRoot = "root"_sd;
    StringData leaf = _leaf;

    std::vector<StringData> result{
        kRoot,
        leaf,
    };

    for (size_t i = 1; i < _leaf.size(); ++i) {
        if (i % _sparsity == 0) {
            result.push_back(leaf.substr(0, i));
        }
    }
    return result;
}

template <typename T>
std::unique_ptr<Edges> getEdgesT(T value, T min, T max, int sparsity) {
    static_assert(!std::numeric_limits<T>::is_signed);
    static_assert(std::numeric_limits<T>::is_integer);

    constexpr size_t bits = std::numeric_limits<T>::digits;

    dassert(0 == min);

    size_t maxlen = getFirstBitSet(max);
    std::string valueBin = toBinaryString(value);
    std::string valueBinTrimmed = valueBin.substr(bits - maxlen, maxlen);
    return std::make_unique<Edges>(valueBinTrimmed, sparsity);
}

std::unique_ptr<Edges> getEdgesInt32(int32_t value,
                                     boost::optional<int32_t> min,
                                     boost::optional<int32_t> max,
                                     int sparsity) {
    auto aost = getTypeInfo32(value, min, max);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity);
}

std::unique_ptr<Edges> getEdgesInt64(int64_t value,
                                     boost::optional<int64_t> min,
                                     boost::optional<int64_t> max,
                                     int sparsity) {
    auto aost = getTypeInfo64(value, min, max);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity);
}

std::unique_ptr<Edges> getEdgesDouble(double value,
                                      boost::optional<double> min,
                                      boost::optional<double> max,
                                      boost::optional<uint32_t> precision,
                                      int sparsity) {
    auto aost = getTypeInfoDouble(value, min, max, precision);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity);
}

std::unique_ptr<Edges> getEdgesDecimal128(Decimal128 value,
                                          boost::optional<Decimal128> min,
                                          boost::optional<Decimal128> max,
                                          boost::optional<uint32_t> precision,
                                          int sparsity) {
    auto aost = getTypeInfoDecimal128(value, min, max, precision);
    return getEdgesT(aost.value, aost.min, aost.max, sparsity);
}


template <typename T>
class MinCoverGenerator {
public:
    static std::vector<std::string> minCover(T lowerBound, T upperBound, T max, int sparsity) {
        MinCoverGenerator<T> mcg(lowerBound, upperBound, max, sparsity);
        std::vector<std::string> c;
        mcg.minCoverRec(c, 0, mcg._maxlen);
        return c;
    }

private:
    MinCoverGenerator(T lowerBound, T upperBound, T max, int sparsity)
        : _lowerBound(lowerBound),
          _upperBound(upperBound),
          _sparsity(sparsity),
          _maxlen(getFirstBitSet(max)) {
        static_assert(!std::numeric_limits<T>::is_signed);
        static_assert(std::numeric_limits<T>::is_integer);
        tassert(6860001,
                "Lower bound must be less or equal to upper bound for range search.",
                lowerBound <= upperBound);
        dassert(lowerBound >= 0 && upperBound <= max);
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

    // Some levels are discarded when sparsity does not divide current level
    // Discarded levels are replaced by the set of edges on the next level
    // Return true if level is stored
    bool isLevelStored(int maskedBits) {
        int level = _maxlen - maskedBits;
        return 0 == maskedBits || 0 == (level % _sparsity);
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
};

template <typename T>
std::vector<std::string> minCover(T lowerBound, T upperBound, T min, T max, int sparsity) {
    dassert(0 == min);
    return MinCoverGenerator<T>::minCover(lowerBound, upperBound, max, sparsity);
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
                                       int sparsity) {
    auto a = getTypeInfo32(lowerBound, min, max);
    auto b = getTypeInfo32(upperBound, min, max);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity);
}

std::vector<std::string> minCoverInt64(int64_t lowerBound,
                                       bool includeLowerBound,
                                       int64_t upperBound,
                                       bool includeUpperBound,
                                       boost::optional<int64_t> min,
                                       boost::optional<int64_t> max,
                                       int sparsity) {
    auto a = getTypeInfo64(lowerBound, min, max);
    auto b = getTypeInfo64(upperBound, min, max);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity);
}

std::vector<std::string> minCoverDouble(double lowerBound,
                                        bool includeLowerBound,
                                        double upperBound,
                                        bool includeUpperBound,
                                        boost::optional<double> min,
                                        boost::optional<double> max,
                                        boost::optional<uint32_t> precision,
                                        int sparsity) {
    auto a = getTypeInfoDouble(lowerBound, min, max, precision);
    auto b = getTypeInfoDouble(upperBound, min, max, precision);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity);
}
std::vector<std::string> minCoverDecimal128(Decimal128 lowerBound,
                                            bool includeLowerBound,
                                            Decimal128 upperBound,
                                            bool includeUpperBound,
                                            boost::optional<Decimal128> min,
                                            boost::optional<Decimal128> max,
                                            boost::optional<uint32_t> precision,
                                            int sparsity) {
    auto a = getTypeInfoDecimal128(lowerBound, min, max, precision);
    auto b = getTypeInfoDecimal128(upperBound, min, max, precision);
    dassert(a.min == b.min);
    dassert(a.max == b.max);
    adjustBounds(a, includeLowerBound, b, includeUpperBound);
    if (a.value > b.value) {
        return {};
    }
    return minCover(a.value, b.value, a.min, a.max, sparsity);
}

PrfBlock FLEUtil::blockToArray(const SHA256Block& block) {
    PrfBlock data;
    memcpy(data.data(), block.data(), sizeof(PrfBlock));
    return data;
}

PrfBlock FLEUtil::prf(ConstDataRange key, ConstDataRange cdr) {
    uassert(6378002, "Invalid key length", key.length() == crypto::sym256KeySize);

    SHA256Block block;
    SHA256Block::computeHmac(key.data<uint8_t>(), key.length(), {cdr}, &block);
    return blockToArray(block);
}

PrfBlock FLEUtil::prf(ConstDataRange key, uint64_t value) {
    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);

    return prf(key, bufValue);
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
}  // namespace mongo
