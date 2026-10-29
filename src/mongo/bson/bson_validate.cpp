// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_validate.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelementvalue.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumn_util.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/str_escape.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

// The values of the kSkipXX styles are used to compute the size, the remaining ones are arbitrary.
// NOTE: The kSkipXX values directly encode the amount of 4-byte words to skip: don't change them!
enum ValidationStyle : uint8_t {
    kSkip0 = 0,          // The element only consists of the type byte and field name.
    kSkip4 = 1,          // There are 4 additional bytes of data, see note above.
    kSkip8 = 2,          // There are 8 additional bytes of data, see note above.
    kSkip12 = 3,         // There are 12 additional bytes of data, see note above.
    kSkip16 = 4,         // There are 16 additional bytes of data, see note above.
    kString = 5,         // An int32 with the string length (including NUL) follows the field name.
    kObjectOrArray = 6,  // The type starts a new nested object or array.
    kSpecial = 7,        // Handled specially: any cases that don't fall into the above.
};

// This table is padded and aligned to 32 bytes for more efficient lookup.
static constexpr ValidationStyle kTypeInfoTable alignas(32)[32] = {
    ValidationStyle::kSpecial,        // \x00 EOO
    ValidationStyle::kSkip8,          // \x01 NumberDouble
    ValidationStyle::kString,         // \x02 String
    ValidationStyle::kObjectOrArray,  // \x03 Object
    ValidationStyle::kObjectOrArray,  // \x04 Array
    ValidationStyle::kSpecial,        // \x05 BinData
    ValidationStyle::kSkip0,          // \x06 Undefined
    ValidationStyle::kSkip12,         // \x07 OID
    ValidationStyle::kSpecial,        // \x08 Bool (requires 0/1 false/true validation)
    ValidationStyle::kSkip8,          // \x09 Date
    ValidationStyle::kSkip0,          // \x0a Null
    ValidationStyle::kSpecial,        // \x0b Regex (two nul-terminated strings)
    ValidationStyle::kSpecial,        // \x0c DBRef
    ValidationStyle::kString,         // \x0d Code
    ValidationStyle::kString,         // \x0e Symbol
    ValidationStyle::kSpecial,        // \x0f CodeWScope
    ValidationStyle::kSkip4,          // \x10 Int
    ValidationStyle::kSkip8,          // \x11 Timestamp
    ValidationStyle::kSkip8,          // \x12 Long
    ValidationStyle::kSkip16,         // \x13 Decimal
};
MONGO_STATIC_ASSERT(sizeof(kTypeInfoTable) == 32);

constexpr ErrorCodes::Error InvalidBSON = ErrorCodes::InvalidBSON;
constexpr ErrorCodes::Error NonConformantBSON = ErrorCodes::NonConformantBSON;
constexpr ErrorCodes::Error InvalidBSONColumn = ErrorCodes::InvalidBSONColumn;

// Internal exception thrown by the uassert* helpers below. It carries the complete failure message
// in `status` and the failing check's description (a short, fixed string naming the check, with no
// document data in it) beside it, so that the description travels with the failure to the enclosing
// catch block without ever being attached to the Status (and so is never serialized to a client)
// and without costing anything on the success path. Never escapes this file: every throw site is
// enclosed by a catch in ValidateBuffer::validate() or
// ColumnValidator::doValidateBSONColumn().
struct BSONValidationException {
    Status status;
    std::string description;
};

// Result of one internal validation step: the Status to report, plus the description of the failing
// check when the failure carried one.
struct ValidationStatus {
    static ValidationStatus OK() {
        return {Status::OK(), boost::none};
    }

    bool isOK() const {
        return status.isOK();
    }

    Status status;
    boost::optional<std::string> description;
};

// Throws `code` (NonConformantBSON, InvalidBSON, or InvalidBSONColumn) so callers can distinguish
// the failing check by code, together with `description` so callers can disambiguate further.
// `description` is the failing check's fixed wording and must not embed any of the document's data;
// per-document data (e.g. an observed length) goes in `detail`, which is appended to the message
// only. The message is therefore "<description>" or "<description>: <detail>".
MONGO_COMPILER_NORETURN void uassertedBSONValidation(ErrorCodes::Error code,
                                                     std::string_view description,
                                                     std::string_view detail = {}) {
    throw BSONValidationException{Status(code,
                                         detail.empty()
                                             ? std::string{description}
                                             : fmt::format("{}: {}", description, detail)),
                                  std::string{description}};
}

// Conditional variant mirroring uassert(): throws uassertedBSONValidation(...) if !cond. The
// trailing arguments are the `description` and optional `detail` of uassertedBSONValidation(),
// passed straight into the failure branch, so a `fmt::format(...)` written there is evaluated only
// when the check actually fails.
#define uassertBSONValidation(code, cond, ...)          \
    do {                                                \
        if (MONGO_unlikely(!(cond))) {                  \
            uassertedBSONValidation(code, __VA_ARGS__); \
        }                                               \
    } while (false)

class DefaultValidator {
public:
    void checkNonConformantElem(const char* ptr, uint32_t offsetToValue, const int8_t type) {}

    void checkDuplicateFieldName() {}

    void popLevel() {}

    BSONValidateModeEnum validateMode() const {
        return BSONValidateModeEnum::kDefault;
    }
};

class ExtendedValidator {
public:
    void checkNonConformantElem(const char* ptr, uint32_t offsetToValue, const int8_t type) {
        // Increments the pointer to the actual element value.
        BSONElementValue bsonElemVal(ptr + offsetToValue);
        switch (type) {
            case stdx::to_underlying(BSONType::binData): {
                const auto binData = bsonElemVal.BinData();
                switch (binData.type) {
                    case BinDataType::Column: {
                        // Check for exceptions when decompressing. The block-based decoder is used
                        // over BSONColumn's iterator API, and the materialized elements are
                        // discarded, as we only care about whether decoding throws.
                        //
                        // The decompressor raises its own uasserts, whose messages carry document
                        // data. Describe them with a single fixed description here, keeping the
                        // decompressor's own message on the Status (and so in the logs) rather
                        // than in the validation results.
                        try {
                            bsoncolumn::DiscardingContainer<BSONElement> elements;
                            bsoncolumn::BSONColumnBlockBased(static_cast<const char*>(binData.data),
                                                             binData.length)
                                .decompress<bsoncolumn::BSONElementMaterializer>(
                                    elements, new BSONElementStorage());
                        } catch (const ExceptionFor<ErrorCategory::ValidationError>& e) {
                            uassertedBSONValidation(
                                e.code(), "BSONColumn decompression failed", e.reason());
                        }
                        break;
                    }
                    case BinDataType::Encrypt:
                        _checkEncryptedBSONValue(binData);
                        break;
                    default:
                        // No additional checks on other BinTypes
                        break;
                }
                break;
            }
        }
    }

    void popLevel() {}

    BSONValidateModeEnum validateMode() const {
        return BSONValidateModeEnum::kExtended;
    }

private:
    void _checkEncryptedBSONValue(const BSONBinData& binData) {
        constexpr uint32_t UUIDLength = 16;
        constexpr int32_t minLength = sizeof(uint8_t) + UUIDLength + sizeof(uint8_t);

        auto len = binData.length;
        // Make sure we can read the subtype byte of the Encrypted BSON Value.
        uassertBSONValidation(
            NonConformantBSON, len, "Encrypted BSON Value is missing its subtype byte");

        // Skip the size bytes and BinData subtype byte to the actual encrypted data.
        auto data = static_cast<const char*>(binData.data);
        uint8_t encryptedBinDataTypeByte = ConstDataView(data).read<LittleEndian<uint8_t>>();
        auto encryptedBinDataType = static_cast<EncryptedBinDataType>(encryptedBinDataTypeByte);
        // Only subtype 1, 2, 6, 7, and 9 can exist in MongoDB collections.
        switch (encryptedBinDataType) {
            case EncryptedBinDataType::kDeterministic:
            case EncryptedBinDataType::kRandom: {
                uassertBSONValidation(
                    NonConformantBSON,
                    len > minLength,
                    "Invalid deterministic or random Encrypted BSON Value length",
                    fmt::format("length {}, expected more than {}", len, minLength));
                break;
            }
            case EncryptedBinDataType::kFLE2UnindexedEncryptedValue:
            case EncryptedBinDataType::kFLE2EqualityIndexedValue:
            case EncryptedBinDataType::kFLE2RangeIndexedValue:
            case EncryptedBinDataType::kFLE2EqualityIndexedValueV2:
            case EncryptedBinDataType::kFLE2RangeIndexedValueV2:
            case EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2:
            case EncryptedBinDataType::kFLE2TextIndexedValue: {
                uassertBSONValidation(
                    NonConformantBSON,
                    len >= minLength,
                    "Invalid FLE2 Encrypted BSON Value length",
                    fmt::format("length {}, expected at least {}", len, minLength));
                int8_t originalBsonTypeByte = ConstDataView(data + sizeof(uint8_t) + UUIDLength)
                                                  .read<LittleEndian<uint8_t>>();
                auto originalBsonType = static_cast<BSONType>(originalBsonTypeByte);
                uassertBSONValidation(
                    NonConformantBSON,
                    isFLE2SupportedType(encryptedBinDataType, originalBsonType),
                    "BSON type is not supported for the Encrypted BSON Value subtype",
                    fmt::format("BSON type '{}', subtype {}",
                                typeName(originalBsonType),
                                fmt::underlying(encryptedBinDataType)));
                break;
            }
            default: {
                uassertedBSONValidation(
                    NonConformantBSON,
                    "Unsupported Encrypted BSON Value type in the collection",
                    fmt::format("type {}", fmt::underlying(encryptedBinDataType)));
            }
        }
    }
};

class FullValidator : private ExtendedValidator {
public:
    FullValidator() noexcept {
        _objFrames.push_back({.type = BSONType::object, .indexCounter = 0});
    }

    void checkNonConformantElem(const char* ptr, uint32_t offsetToValue, const int8_t type) {
        // Validate that array indices are monotonically increasing base-10 strings, and field
        // names are UTF8 strings.
        _checkFieldName(ptr);

        ExtendedValidator::checkNonConformantElem(ptr, offsetToValue, type);
        // Increments the pointer to the actual element value.
        const BSONElementValue bsonElemVal(ptr + offsetToValue);
        switch (type) {
            case stdx::to_underlying(BSONType::array):
            case stdx::to_underlying(BSONType::object):
                _objFrames.push_back({.type = BSONType(type), .indexCounter = 0});
                break;
            case stdx::to_underlying(BSONType::binData): {
                const auto binData = bsonElemVal.BinData();
                switch (binData.type) {
                    case BinDataType::newUUID: {
                        constexpr int32_t UUIDLength = 16;
                        auto l = binData.length;
                        uassertBSONValidation(NonConformantBSON,
                                              l == UUIDLength,
                                              "BSON UUID length should be 16 bytes",
                                              fmt::format("found {} instead", l));
                        break;
                    }
                    case BinDataType::MD5Type: {
                        constexpr int32_t md5Length = 16;
                        auto l = binData.length;
                        uassertBSONValidation(NonConformantBSON,
                                              l == md5Length,
                                              "MD5 must be 16 bytes",
                                              fmt::format("got {} instead", l));
                        break;
                    }
                    case BinDataType::ByteArrayDeprecated:
                    case BinDataType::bdtUUID:
                        uassertedBSONValidation(NonConformantBSON,
                                                "Use of deprecated BSON binary data subtype",
                                                fmt::format("{} ({})",
                                                            typeName(BinDataType(binData.type)),
                                                            binData.type));
                        break;
                    default:
                        break;
                }
                break;
            }
            case stdx::to_underlying(BSONType::string):
                // Increment pointer to actual value and then four more to skip size.
                _checkUTF8Char(bsonElemVal.String());
                break;
            case stdx::to_underlying(BSONType::regEx):
                _checkRegexOptions(bsonElemVal);
                break;
            case stdx::to_underlying(BSONType::undefined):
            case stdx::to_underlying(BSONType::dbRef):
            case stdx::to_underlying(BSONType::symbol):
            case stdx::to_underlying(BSONType::codeWScope):
                uassertedBSONValidation(NonConformantBSON,
                                        "Use of deprecated BSON type",
                                        fmt::format("{} ({})", typeName(BSONType(type)), type));
                break;
        }
    }

    void popLevel() {
        if (_objFrames.size() > 0) {
            if (_inObj()) {
                // This was benchmarked against the alternative of using set, unordered_set, and
                // flat_hash_set, it was found to be significantly more performant. For the
                // typical case, BSON documents are expected to be conformant and have unique
                // keys, so the earlier detection permitted by using a set data structure is not
                // expected to be beneficial.
                auto& cur = _objFrames.back();
                std::sort(cur.fieldNames.begin(), cur.fieldNames.end());
                const auto dup = std::adjacent_find(cur.fieldNames.begin(), cur.fieldNames.end());
                uassertBSONValidation(NonConformantBSON,
                                      dup == cur.fieldNames.end(),
                                      "Duplicate key found; element names must be unique",
                                      fmt::format("\"{}\"", *dup));
            }
            _objFrames.pop_back();
        }
    }

    BSONValidateModeEnum validateMode() const {
        return BSONValidateModeEnum::kFull;
    }

private:
    struct ObjectFrame {
        BSONType type;
        DecimalCounter<uint32_t> indexCounter;
        std::vector<std::string_view> fieldNames;
    };

    void _checkUTF8Char(std::string_view str) {
        uassertBSONValidation(NonConformantBSON,
                              str::validUTF8(str),
                              "Found string that doesn't follow UTF-8 encoding");
    }

    bool _inArr() const {
        return _objFrames.size() > 0 && _objFrames.back().type == BSONType::array;
    }

    bool _inObj() const {
        return _objFrames.size() > 0 && _objFrames.back().type == BSONType::object;
    }

    void _checkFieldName(const char* ptr) {
        if (_inArr()) {
            // Checks the actual index field value, starting after the type byte
            const std::string_view actualIndex(ptr + sizeof(char));
            uassertBSONValidation(NonConformantBSON,
                                  _objFrames.back().indexCounter == actualIndex,
                                  "Indices of BSON Array are invalid",
                                  fmt::format("expected {}, but got {}",
                                              std::string_view(_objFrames.back().indexCounter),
                                              actualIndex));
            ++_objFrames.back().indexCounter;
        } else if (_inObj()) {
            const std::string_view fieldName(ptr + sizeof(char));
            _checkUTF8Char(fieldName);
            _objFrames.back().fieldNames.push_back(fieldName);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    void _checkRegexOptions(const BSONElementValue& regex) {
        // Checks that the options are in ascending alphabetical order and that they're all
        // valid.
        static constexpr std::string_view validRegexOptions("ilmsux");
        const std::string_view opt = regex.RegexFlags();
        uassertBSONValidation(NonConformantBSON,
                              opt.find_first_not_of(validRegexOptions) == std::string::npos,
                              "Bad regex options: contains an option that is not allowed",
                              fmt::format("{:?}, only {:?} allowed", opt, validRegexOptions));
        uassertBSONValidation(NonConformantBSON,
                              std::is_sorted(opt.begin(), opt.end()),
                              "Bad regex options: options must be sorted",
                              fmt::format("{:?}", opt));
    }

    // Behaves like a stack, used to validate array index count.
    std::vector<ObjectFrame> _objFrames;
};

template <bool precise>
ValidationStatus _doValidateColumn(const char* originalBuffer,
                                   uint64_t maxLength,
                                   BSONValidateModeEnum mode,
                                   ValidationVersion validationVersion);

template <bool precise, typename BSONValidator>
class ValidateBuffer {
public:
    ValidateBuffer(const char* data,
                   uint64_t maxLength,
                   BSONValidator validator,
                   ValidationVersion validationVersion,
                   bool insideColumn = false)
        : _data(data),
          _maxLength(maxLength),
          _validator(validator),
          _validationVersion(validationVersion),
          _insideColumn(insideColumn) {
        if constexpr (precise)
            _frames.resize(BSONDepth::getMaxAllowableDepth() + 1);
    }

    ValidationStatus validate() noexcept {
        try {
            setupValidation();
            uassertBSONValidation(
                InvalidBSON, _maxLength >= 5, "BSON buffer has to be at least 5 bytes");

            // Read the length as signed integer, to ensure we limit it to < 2GB.
            // All other lengths are read as unsigned, which makes for easier bounds checking.
            Cursor cursor = {_data, _data + _maxLength};
            int32_t len = cursor.template read<int32_t>();
            uassertBSONValidation(InvalidBSON,
                                  len >= 5,
                                  "BSON data has to be at least 5 bytes",
                                  fmt::format("decoded length {}", len));
            uassertBSONValidation(InvalidBSON,
                                  static_cast<size_t>(len) <= _maxLength,
                                  "BSON length exceeds buffer size",
                                  fmt::format("length {} should be less or equal to {}",
                                              static_cast<size_t>(len),
                                              _maxLength));
            const char* end = _currFrame->end = _data + len;
            uassertBSONValidation(InvalidBSON, end[-1] == 0, "BSON object not terminated with EOO");
            _validateIterative(Cursor{cursor.ptr, end});
        } catch (const BSONValidationException& e) {
            // The description travels beside the Status rather than on it, so that it is never
            // serialized to a client.
            return {
                Status(e.status.code(), str::stream() << e.status.reason() << " " << _context()),
                e.description};
        } catch (const ExceptionFor<ErrorCategory::ValidationError>& e) {
            // A validation failure raised by code outside this file, which carries no description.
            return {Status(e.code(), str::stream() << e.what() << " " << _context()), boost::none};
        }
        return ValidationStatus::OK();
    }

    /* Assumes the root level is a single literal element (which may contain nested objects).
     * Only validates up to the termination of that first literal, more data is permitted to
     * remain in the buffer after that and is not validated. Throws exception on invalid data.
     * Confirm field names for literals in BSONColumn have empty field names.
     */
    int validateAndMeasureElem() {
        setupValidation();
        uassertBSONValidation(InvalidBSON,
                              _maxLength > 1,  // must at least have a 0-terminator after control
                              "BSON literal is not followed by fieldname");
        // Confirm fieldName is just a null terminator
        uassertBSONValidation(NonConformantBSON,
                              _maxLength > 1 && _data[1] == 0,
                              "BSON literal content does not have an empty fieldname");

        // Handle one element without using iterative loop, and without expecting
        // multiple instances or an EOO.  Only resume with the iterative loop if
        // we have nested objects
        _currElem = _data;
        const int8_t type = ConstDataView(_data).read<int8_t>();
        const char* ptr = _validateElem<false>(Cursor{_data + 2, _data + _maxLength}, type);
        _validator.checkNonConformantElem(_data, 2, type);

        if (_firstFrameUpdated) {
            // We know that type was kObject/kArray/kCodeWScope
            // Size is fieldname, type, and a stored int
            int64_t size =
                static_cast<int64_t>(ConstDataView(_data + 2).read<LittleEndian<int32_t>>()) + 2;
            uassertBSONValidation(InvalidBSON,
                                  (size_t)size <= _maxLength,
                                  "BSON literal content exceeds buffer size");
            _validateIterative(Cursor{ptr, _data + size});
            return size;
        } else {
            return ptr - _data;
        }
    }

private:
    struct Empty {};

    void inline setupValidation() {
        _currFrame = _frames.begin();
        _currElem = nullptr;
        auto maxFrames = BSONDepth::getMaxAllowableDepth() + 1;  // A flat BSON has one frame.
        uassertBSONValidation(
            InvalidBSON, _frames.size() <= maxFrames, "Cannot enforce max nesting depth");
    }

    /**
     * Extra information for each nesting level in the precise validation mode.
     */
    struct PreciseFrameInfo {
        // Points to the start (type byte) of the element that started this nesting level
        // (Object, Array, or CodeWScope). An exception to this case is the CodeWScope scope
        // frame (see _pushCodeWithScope), where nestedElemStart points to the NUL terminator
        // of the code string.
        const char* nestedElemStart = nullptr;
    };

    struct Frame : public std::conditional<precise, PreciseFrameInfo, Empty>::type {
        const char* end;  // Used for checking encoded object/array sizes, not bounds checking.
    };

    using Frames =
        typename std::conditional<precise, std::vector<Frame>, std::array<Frame, 32>>::type;

    struct Cursor {
        /* Also requires remaining buf after the skip (both BSONColumn and BSONObj guarantee
           this by having at minimum a trailing EOO) */
        void skip(size_t len) {
            uassertBSONValidation(InvalidBSON,
                                  (ptr += len) < end,
                                  "BSON element value extends past the end of the buffer");
        }

        template <typename T>
        T read() {
            auto val = ptr;
            skip(sizeof(T));
            return ConstDataView(val).read<LittleEndian<T>>();
        }

        void skipString() {
            auto len = read<uint32_t>();
            skip(len);
            uassertBSONValidation(InvalidBSON, !ptr[-1] && len > 0, "Not null terminated string");
        }

        size_t strlen() const {
            // This is actually by far the hottest code in all of BSON validation.
            dassert(ptr < end);
            size_t len = 0;
            while (ptr[len])
                ++len;
            return len;
        }

        const char* ptr;
        const char* const end;
    };

    const char* _pushFrame(Cursor cursor) {
        uassertBSONValidation(ErrorCodes::Overflow,
                              ++_currFrame != _frames.end(),
                              "BSONObj exceeds maximum nested object depth");
        return _updateFrame(cursor);
    }

    const char* _updateFrame(Cursor cursor) {
        auto obj = cursor.ptr;
        auto len = cursor.template read<int32_t>();
        uassertBSONValidation(InvalidBSON,
                              len >= 5,
                              "Nested BSON object has to be at least 5 bytes",
                              fmt::format("decoded length {}", len));
        _currFrame->end = obj + len;

        if constexpr (precise) {
            _currFrame->nestedElemStart = _currElem;
        }
        return cursor.ptr;
    }

    bool _popFrame() {
        if (_currFrame == _frames.begin())
            return false;
        --_currFrame;
        _validator.popLevel();
        return true;
    }

    const char* _validateSpecial(Cursor cursor, int8_t type) {
        switch (type) {
            case stdx::to_underlying(BSONType::binData): {
                auto count = cursor.template read<uint32_t>();
                auto subtype = cursor.template read<uint8_t>();
                const char* columnStart = cursor.ptr;
                cursor.skip(count);
                if (subtype == BinDataType::Column && _validationVersion >= V2_Column) {
                    uassertBSONValidation(InvalidBSONColumn,
                                          !_insideColumn,
                                          "BSONColumn cannot contain nested BSONColumn data");
                    /* do not pass down cursor; we want to reset the nesting depth */
                    if (auto columnResult = _doValidateColumn<precise>(
                            columnStart, count, _validator.validateMode(), _validationVersion);
                        MONGO_unlikely(!columnResult.isOK())) {
                        // The inner column validation already described the specific check that
                        // failed; keep that description rather than replacing it with the coarser
                        // "Invalid BSON column", so the caller can still tell where in the column
                        // code the failure originated. (The code is still reported as
                        // NonConformantBSON: column errors are wrapped at this boundary.)
                        uassertedBSONValidation(
                            NonConformantBSON,
                            std::move(columnResult.description).value_or("Invalid BSON column"),
                            columnResult.status.reason());
                    }
                }
                break;
            }
            case stdx::to_underlying(BSONType::boolean):
                if (auto value = cursor.template read<uint8_t>())  // If not 0, must be 1.
                    uassertBSONValidation(
                        InvalidBSON, value == 1, "BSON bool is neither false nor true");
                break;
            case stdx::to_underlying(BSONType::regEx):
                cursor.skip(0);  // Force validation of the ptr after skipping past the field name.
                cursor.skip(cursor.strlen() + 1);  // Skip regular expression cstring.
                cursor.skip(cursor.strlen() + 1);  // Skip options cstring.
                break;
            case stdx::to_underlying(BSONType::dbRef):
                cursor.skipString();  // Like String, but...
                cursor.skip(12);      // ...also skip the 12-byte ObjectId.
                break;
            case stdx::to_underlying(BSONType::minKey):
            case stdx::to_underlying(BSONType::maxKey):
                cursor.skip(0);  // Force validation of the ptr after skipping past the field name.
                break;
            default:
                uassertedBSONValidation(
                    InvalidBSON, "Unrecognized BSON type", fmt::format("type {}", type));
        }
        return cursor.ptr;
    }

    template <bool nestedFrame>
    const char* _pushCodeWithScope(Cursor cursor) {
        // Push a dummy frame to check the CodeWScope size.
        if constexpr (nestedFrame)
            cursor.ptr = _pushFrame(cursor);
        else
            cursor.ptr = _updateFrame(cursor);
        cursor.skipString();         // Now skip the BSON UTF8 string containing the code.
        _currElem = cursor.ptr - 1;  // Use the terminating NUL as a dummy scope element.
        return _pushFrame(cursor);
    }

    void _maybePopCodeWithScope(Cursor cursor) {
        if constexpr (precise) {
            // When ending the scope of a CodeWScope, pop the extra dummy frame and check its
            // size.
            if (_currFrame != _frames.begin()) {
                if (auto nestedElemStart = (_currFrame - 1)->nestedElemStart;
                    nestedElemStart != nullptr &&
                    static_cast<BSONType>(ConstDataView(nestedElemStart).read<int8_t>()) ==
                        BSONType::codeWScope) {
                    invariant(_popFrame());
                    uassertBSONValidation(InvalidBSON,
                                          cursor.ptr == _currFrame->end,
                                          "Incorrect BSON length in CodeWScope");
                }
            }
        }
    }

    template <bool nestedFrame>
    const char* _validateElem(Cursor cursor, int8_t type) {
        if (MONGO_unlikely(type < 0 || type > stdx::to_underlying(BSONType::jsTypeMax)))
            return _validateSpecial(cursor, type);

        auto style = kTypeInfoTable[type];
        if (MONGO_likely(style <= kSkip16)) {
            cursor.skip(style * 4);
        } else if (MONGO_likely(style == kString)) {
            cursor.skipString();
        } else if (MONGO_likely(style == kObjectOrArray)) {
            if constexpr (nestedFrame) {
                cursor.ptr = _pushFrame(cursor);
            } else {
                cursor.ptr = _updateFrame(cursor);
                _firstFrameUpdated = true;
            }
        } else if (MONGO_unlikely(precise && type == stdx::to_underlying(BSONType::codeWScope))) {
            cursor.ptr = _pushCodeWithScope<nestedFrame>(cursor);
            if constexpr (!nestedFrame)
                _firstFrameUpdated = true;
        } else {
            cursor.ptr = _validateSpecial(cursor, type);
        }

        return cursor.ptr;
    }

    MONGO_COMPILER_NOINLINE void _validateIterative(Cursor cursor) {
        do {
            // Use the fact that the EOO byte is 0, just like the end of string, so checking for
            // EOO is same as finding len == 0. The cursor cannot point past EOO, so the strlen
            // is safe.
            uassertBSONValidation(InvalidBSON,
                                  cursor.ptr < cursor.end,
                                  "BSON element starts past the end of the buffer");
            while (size_t len = cursor.strlen()) {
                const int8_t type = ConstDataView(cursor.ptr).read<int8_t>();
                _currElem = cursor.ptr;
                // In case _currElem is moved (for instance when the type is CodeWScope).
                auto elemStart = cursor.ptr;
                cursor.ptr += len + 1;
                cursor.ptr = _validateElem<true>(cursor, type);

                // Check if the data is compliant to other BSON specifications if the element is
                // structurally correct.
                _validator.checkNonConformantElem(elemStart, len + 1, type);

                if constexpr (precise) {
                    // See if the _id field was just validated. If so, capture it for error context.
                    if (_currFrame == _frames.begin() && std::string_view(_currElem + 1) == "_id"sv)
                        _id = BSONElement(_currElem);  // This is fully validated now.
                }
                dassert(cursor.ptr < cursor.end);
            }

            // Got the EOO byte: skip it and compare its location with the expected frame end.
            uassertBSONValidation(InvalidBSON,
                                  ++cursor.ptr == _currFrame->end,
                                  "Incorrect BSON length: EOO is not at the end of the object");
            _maybePopCodeWithScope(cursor);
        } while (_popFrame());  // Finished when there are no frames left.

        _validator.popLevel();
    }

    /**
     * Returns a string qualifying the context in which an exception occurred. Example return is
     * "in element with field name 'foo.bar' in object with _id: 1".
     */
    std::string _context() {
        str::stream ctx;
        ctx << "in element with field name '";
        if constexpr (precise) {
            std::for_each(
                _frames.begin() + 1, _currFrame + (_currFrame != _frames.end()), [&](auto& frame) {
                    // `nestedElemStart` is the type byte of the Object/Array/CodeWScope that
                    // opened this frame. Skip when it was never assigned.
                    // Also skip the CodeWScope *scope* frame (see _pushCodeWithScope), which
                    // stores the code string's null terminator as a dummy element.
                    if (frame.nestedElemStart && *frame.nestedElemStart) {
                        // Field name was validated for null termination before this frame was
                        // pushed.
                        ctx << frame.nestedElemStart + 1 << ".";
                    }
                });
        }
        // Also prints "?" for CodeWScope scope frame, see comment above.
        ctx << (_currElem && *_currElem ? _currElem + 1 : "?") << "'";

        if constexpr (precise) {
            ctx << " in object with " << (_id ? _id.toString() : "unknown _id");
        }
        return str::escape(ctx);
    }

    const char* const _data;  // The data buffer to check.
    const size_t _maxLength;  // The size of the data buffer. The BSON object may be smaller.
    const char* _currElem = nullptr;  // Element to validate: only the name is known to be good.
    typename Frames::iterator _currFrame;  // Frame currently being validated.
    Frames _frames;   // Has end pointers to check and the containing element for precise mode.
    BSONElement _id;  // The _id element of the root object. Used in error context string.
    BSONValidator _validator;
    ValidationVersion _validationVersion;
    bool _insideColumn;
    bool _firstFrameUpdated = false;  // Has the first frame received nested while measuring an elem
};

template <typename BSONValidator>
ValidationStatus _doValidate(const char* originalBuffer,
                             uint64_t maxLength,
                             BSONValidator validator,
                             ValidationVersion validationVersion) {
    // First try validating using the fast but less precise version. That version will return
    // a not-OK status for objects with CodeWScope or nesting exceeding 32 levels. These cases
    // and actual failures will rerun the precise version that gives a detailed error context.
    if (MONGO_likely((ValidateBuffer<false, BSONValidator>(
                          originalBuffer, maxLength, validator, validationVersion, false)
                          .validate()
                          .isOK())))
        return ValidationStatus::OK();

    return ValidateBuffer<true, BSONValidator>(
               originalBuffer, maxLength, validator, validationVersion, false)
        .validate();
}

template <bool precise>
class ColumnValidator {
public:
    static ValidationStatus doValidateBSONColumn(const char* originalBuffer,
                                                 int maxLength,
                                                 BSONValidateModeEnum mode,
                                                 ValidationVersion validationVersion) noexcept {
        // run control pointer through to end of buffer
        // run over literal data as directed by lengths from control
        // check formatting of Simple8B blocks
        // scan reference objects of interleaved mode starts
        // confirm EOO terminations of interleaved modes
        // content of interleaved objects does not need to be checked differently from
        //      standard Simple8B block and literal decodings
        // confirm we end at end of buffer
        const char* ptr = originalBuffer;
        const char* end = originalBuffer + maxLength;
        bool interleavedMode = false;

        try {
            // Check this beforehand to ensure we cannot overflow the buffer with any strlen
            uassertBSONValidation(NonConformantBSON,
                                  ptr < end && *(end - 1) == stdx::to_underlying(BSONType::eoo),
                                  "BSON column is missing EOO termination");

            while (ptr < end) {
                uint8_t control = *ptr;
                if (control == stdx::to_underlying(BSONType::eoo)) {
                    ptr++;
                    if (interleavedMode) {
                        interleavedMode = false;
                    } else {
                        // should be the last control of the sequence
                        uassertBSONValidation(NonConformantBSON,
                                              ptr == end,
                                              "BSONColumn EOO does not fully consume buffer");
                        return ValidationStatus::OK();
                    }
                } else if (bsoncolumn::isUncompressedLiteralControlByte(control)) {
                    int size;
                    if (MONGO_likely(mode == BSONValidateModeEnum::kDefault))
                        size = ValidateBuffer<precise, DefaultValidator>(
                                   ptr, end - ptr, DefaultValidator(), validationVersion, true)
                                   .validateAndMeasureElem();
                    else if (mode == BSONValidateModeEnum::kExtended)
                        size = ValidateBuffer<precise, ExtendedValidator>(
                                   ptr, end - ptr, ExtendedValidator(), validationVersion, true)
                                   .validateAndMeasureElem();
                    else if (mode == BSONValidateModeEnum::kFull)
                        size = ValidateBuffer<precise, FullValidator>(
                                   ptr, end - ptr, FullValidator(), validationVersion, true)
                                   .validateAndMeasureElem();
                    else
                        MONGO_UNREACHABLE;

                    ptr += size;
                } else if (bsoncolumn::isInterleavedStartControlByte(control)) {
                    // interleaved objects begin with a reference object, and then a series
                    // of diff blocks for followup objects, ending with an EOO. Nesting
                    // interleaved mode is not allowed.
                    uassertBSONValidation(
                        InvalidBSONColumn, !interleavedMode, "Nested interleaved mode");
                    ++ptr;
                    const auto validateResult = [&] {
                        if (MONGO_likely(mode == BSONValidateModeEnum::kDefault))
                            return ValidateBuffer<precise, DefaultValidator>(
                                       ptr, end - ptr, DefaultValidator(), validationVersion, true)
                                .validate();
                        if (mode == BSONValidateModeEnum::kExtended)
                            return ValidateBuffer<precise, ExtendedValidator>(
                                       ptr, end - ptr, ExtendedValidator(), validationVersion, true)
                                .validate();
                        if (mode == BSONValidateModeEnum::kFull)
                            return ValidateBuffer<precise, FullValidator>(
                                       ptr, end - ptr, FullValidator(), validationVersion, true)
                                .validate();
                        MONGO_UNREACHABLE;
                    }();
                    uassertBSONValidation(InvalidBSONColumn,
                                          validateResult.isOK(),
                                          "Invalid reference object for interleaved mode",
                                          validateResult.status.reason());
                    // we now know the reference object is valid and safe to interpret
                    BSONObj reference(ptr);
                    ptr += reference.objsize();
                    interleavedMode = true;
                } else {
                    // Simple8b block sequence, just check for memory overflow of block count
                    uint8_t numBlocks = bsoncolumn::numSimple8bBlocksForControlByte(control);
                    int size = sizeof(uint64_t) * numBlocks;
                    uassertBSONValidation(InvalidBSONColumn,
                                          ptr + size + 1 <= end,
                                          "BSONColumn blocks exceed buffer size");
                    ptr += 1 + size;
                }
            }
        } catch (const BSONValidationException& e) {
            // The description travels beside the Status rather than on it, so that it is never
            // serialized to a client.
            return {e.status, e.description};
        } catch (const ExceptionFor<ErrorCategory::ValidationError>& e) {
            // A validation failure raised by code outside this file, which carries no description.
            return {e.toStatus(), boost::none};
        }

        // We should not get here for a valid object, the final EOO should have returned OK. This is
        // a direct return rather than a throw, so report the description the way the uassert*
        // helpers would.
        constexpr auto kMissingTerminatingEoo = "Missing terminating EOO"sv;
        return {Status(NonConformantBSON, kMissingTerminatingEoo),
                std::string{kMissingTerminatingEoo}};
    }
};

template <bool precise>
ValidationStatus _doValidateColumn(const char* originalBuffer,
                                   uint64_t maxLength,
                                   BSONValidateModeEnum mode,
                                   ValidationVersion validationVersion) {
    if constexpr (precise) {
        // First try validating using the fast but less precise version. That version will
        // return a not-OK status for objects with CodeWScope or nesting exceeding 32 levels.
        // These cases and actual failures will rerun the precise version that gives a detailed
        // error context.
        if (MONGO_likely(ColumnValidator<false>::doValidateBSONColumn(
                             originalBuffer, maxLength, mode, validationVersion)
                             .isOK()))
            return ValidationStatus::OK();

        return ColumnValidator<true>::doValidateBSONColumn(
            originalBuffer, maxLength, mode, validationVersion);
    } else {
        return ColumnValidator<false>::doValidateBSONColumn(
            originalBuffer, maxLength, mode, validationVersion);
    }
}

}  // namespace

Status validateBSON(const char* originalBuffer,
                    uint64_t maxLength,
                    BSONValidateModeEnum mode,
                    ValidationVersion validationVersion,
                    boost::optional<std::string>* outDescription) noexcept {
    auto result = [&] {
        if (MONGO_likely(mode == BSONValidateModeEnum::kDefault))
            return _doValidate(originalBuffer, maxLength, DefaultValidator(), validationVersion);
        else if (mode == BSONValidateModeEnum::kExtended)
            return _doValidate(originalBuffer, maxLength, ExtendedValidator(), validationVersion);
        else if (mode == BSONValidateModeEnum::kFull)
            return ValidateBuffer<true, FullValidator>(
                       originalBuffer, maxLength, FullValidator(), validationVersion, false)
                .validate();
        else
            MONGO_UNREACHABLE;
    }();

    // The failing check reported its description beside the Status rather than on it, so that the
    // success path pays nothing and the description is never serialized to a client.
    if (MONGO_unlikely(outDescription && !result.isOK())) {
        *outDescription = std::move(result.description);
    }
    return result.status;
}

Status validateBSON(const BSONObj& obj,
                    BSONValidateModeEnum mode,
                    ValidationVersion validationVersion,
                    boost::optional<std::string>* outDescription) noexcept {
    return validateBSON(obj.objdata(), obj.objsize(), mode, validationVersion, outDescription);
}

Status validateBSONColumn(const char* originalBuffer,
                          int maxLength,
                          BSONValidateModeEnum mode,
                          ValidationVersion validationVersion) noexcept {
    return _doValidateColumn<true>(originalBuffer, maxLength, mode, validationVersion).status;
}

void uassertValidBSONFromJavaScript(const BSONObj& obj, std::string_view context) {
    if (auto status = validateBSON(obj); !status.isOK()) {
        std::string ctx{context};
        uasserted(ErrorCodes::InvalidBSONFromJavaScript,
                  str::stream() << ctx << ": " << status.toString());
    }
}

namespace {

/**
 * Shared implementation of validateBSONDepthForUserStorage. If not a nullptr, 'topLevelVisitor' is
 * invoked for every top-level element of 'obj' while the depth validation traversal is already
 * visiting those elements. Does not return a status, but throws when encountering validation
 * errors. If not a nullptr, the visitor is expected to be cheap (or a no-op) because it is called
 * once per top-level field.
 */
template <typename F>
void validateBSONDepthForUserStorageImpl(const BSONObj& obj, const F& topLevelVisitor) {
    absl::InlinedVector<BSONObjIterator, 32> frames;
    frames.emplace_back(obj);

    while (!frames.empty()) {
        if (!frames.back().more()) {
            frames.pop_back();
            continue;
        }

        const auto elem = frames.back().next();
        if constexpr (!std::is_same_v<F, std::nullptr_t>) {
            if (frames.size() == 1) {
                topLevelVisitor(elem);
            }
        }

        if (elem.type() == BSONType::object || elem.type() == BSONType::array) {
            if (auto subObj = elem.embeddedObject(); !subObj.isEmpty()) {
                // Empty subdocuments do not count toward the depth of a document.
                const auto maxDepth = BSONDepth::getMaxDepthForUserStorage();
                uassert(ErrorCodes::Overflow,
                        fmt::format("object exceeds {} levels of nesting", maxDepth),
                        frames.size() < maxDepth);
                frames.emplace_back(subObj);
            }
        }
    }
}

}  // namespace

Status validateBSONDepthForUserStorage(const BSONObj& obj) {
    try {
        validateBSONDepthForUserStorageImpl(obj, nullptr);
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Status validateBSONDepthForUserStorage(
    const BSONObj& obj, const std::function<void(const BSONElement&)>& topLevelVisitor) {
    try {
        validateBSONDepthForUserStorageImpl(obj, topLevelVisitor);
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

}  // namespace mongo
