// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_view.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/oid.h"
#include "mongo/platform/decimal128.h"

#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace mongo {

namespace {

/**
 * Creates a status with InvalidBSON code and adds information about _id if available.
 * WARNING: only pass in a non-EOO idElem if it has been fully validated already!
 * 'elemName' should be the known, validated field name of the element containing the error, if it
 * exists. Otherwise, it should be empty.
 */
MONGO_COMPILER_NOINLINE Status makeError(std::string_view baseMsg,
                                         BSONElement idElem,
                                         std::string_view elemName) {
    str::stream msg;
    msg << baseMsg;

    if (!elemName.empty()) {
        msg << " in element with field name '";
        msg << elemName;
        msg << "'";
    }

    if (idElem.eoo()) {
        msg << " in object with unknown _id";
    } else {
        msg << " in object with " + idElem.toString(/*field name=*/true, /*full=*/true);
    }
    return Status(ErrorCodes::InvalidBSON, msg);
}

class Buffer {
public:
    Buffer(const char* buffer, uint64_t maxLength)
        : _buffer(buffer), _position(0), _maxLength(maxLength) {}

    template <typename N>
    bool readNumber(N* out) {
        if ((_position + sizeof(N)) > _maxLength)
            return false;
        if (out) {
            *out = ConstDataView(_buffer).read<LittleEndian<N>>(_position);
        }
        _position += sizeof(N);
        return true;
    }

    /* Attempts to read a c-string starting at the next position in the buffer, and writes the
     * string into 'out', if non-null.
     * 'elemName' should be the known, validated field name of the BSONElement in which we are
     * reading, if it exists. Otherwise, it should be empty.
     */
    Status readCString(std::string_view elemName, std::string_view* out) {
        const void* x = memchr(_buffer + _position, 0, _maxLength - _position);
        if (!x)
            return makeError("no end of c-string", _idElem, elemName);
        uint64_t len = static_cast<uint64_t>(static_cast<const char*>(x) - (_buffer + _position));

        std::string_view data(_buffer + _position, len);
        _position += len + 1;

        if (out) {
            *out = data;
        }
        return Status::OK();
    }

    /* Attempts to read a UTF8 string starting at the next position in the buffer, and writes the
     * string into 'out', if non-null.
     * 'elemName' should be the known, validated field name of the BSONElement in which we are
     * reading, if it exists. Otherwise, it should be empty.
     */
    Status readUTF8String(std::string_view elemName, std::string_view* out) {
        int sz;
        if (!readNumber<int>(&sz))
            return makeError("invalid bson", _idElem, elemName);

        if (sz <= 0) {
            // must have NULL at the very least
            return makeError("invalid bson", _idElem, elemName);
        }

        if (out) {
            *out = std::string_view(_buffer + _position, sz);
        }

        if (!skip(sz - 1))
            return makeError("invalid bson", _idElem, elemName);

        char c;
        if (!readNumber<char>(&c))
            return makeError("invalid bson", _idElem, elemName);

        if (c != 0)
            return makeError("not null terminated string", _idElem, elemName);

        return Status::OK();
    }

    bool skip(uint64_t sz) {
        _position += sz;
        return _position < _maxLength;
    }

    uint64_t position() const {
        return _position;
    }

    const char* getBasePtr() const {
        return _buffer;
    }

    /**
     * WARNING: only pass in a non-EOO idElem if it has been fully validated already!
     */
    void setIdElem(BSONElement idElem) {
        _idElem = idElem;
    }

private:
    const char* _buffer;
    uint64_t _position;
    uint64_t _maxLength;
    BSONElement _idElem;
};

struct ValidationState {
    enum State { BeginObj = 1, WithinObj, EndObj, BeginCodeWScope, EndCodeWScope, Done };
};

class ValidationObjectFrame {
public:
    int startPosition() const {
        return _startPosition & ~(1 << 31);
    }
    bool isCodeWithScope() const {
        return _startPosition & (1 << 31);
    }

    void setStartPosition(int pos) {
        _startPosition = (_startPosition & (1 << 31)) | (pos & ~(1 << 31));
    }
    void setIsCodeWithScope(bool isCodeWithScope) {
        if (isCodeWithScope) {
            _startPosition |= 1 << 31;
        } else {
            _startPosition &= ~(1 << 31);
        }
    }

    int expectedSize;

private:
    int _startPosition;
};

/**
 * WARNING: only pass in a non-EOO idElem if it has been fully validated already!
 */
Status validateElementInfo(Buffer* buffer,
                           ValidationState::State* nextState,
                           BSONElement idElem,
                           std::string_view* elemName) {
    Status status = Status::OK();

    signed char type;
    if (!buffer->readNumber<signed char>(&type))
        return makeError("invalid bson", idElem, std::string_view());

    if (type == stdx::to_underlying(BSONType::eoo)) {
        *nextState = ValidationState::EndObj;
        return Status::OK();
    }

    status = buffer->readCString(std::string_view(), elemName);
    if (!status.isOK())
        return status;

    switch (type) {
        case stdx::to_underlying(BSONType::minKey):
        case stdx::to_underlying(BSONType::maxKey):
        case stdx::to_underlying(BSONType::null):
        case stdx::to_underlying(BSONType::undefined):
            return Status::OK();

        case stdx::to_underlying(BSONType::oid):
            if (!buffer->skip(OID::kOIDSize))
                return makeError("invalid bson", idElem, *elemName);
            return Status::OK();

        case stdx::to_underlying(BSONType::numberInt):
            if (!buffer->skip(sizeof(int32_t)))
                return makeError("invalid bson", idElem, *elemName);
            return Status::OK();

        case stdx::to_underlying(BSONType::boolean):
            uint8_t val;
            if (!buffer->readNumber(&val))
                return makeError("invalid bson", idElem, *elemName);
            if ((val != 0) && (val != 1))
                return makeError("invalid boolean value", idElem, *elemName);
            return Status::OK();

        case stdx::to_underlying(BSONType::numberDouble):
        case stdx::to_underlying(BSONType::numberLong):
        case stdx::to_underlying(BSONType::timestamp):
        case stdx::to_underlying(BSONType::date):
            if (!buffer->skip(sizeof(int64_t)))
                return makeError("invalid bson", idElem, *elemName);
            return Status::OK();

        case stdx::to_underlying(BSONType::numberDecimal):
            if (!buffer->skip(sizeof(Decimal128::Value)))
                return makeError("Invalid bson", idElem, *elemName);
            return Status::OK();

        case stdx::to_underlying(BSONType::dbRef):
            status = buffer->readUTF8String(*elemName, nullptr);
            if (!status.isOK())
                return status;
            if (!buffer->skip(OID::kOIDSize)) {
                return makeError("invalid bson length", idElem, *elemName);
            }
            return Status::OK();

        case stdx::to_underlying(BSONType::regEx):
            status = buffer->readCString(*elemName, nullptr);
            if (!status.isOK())
                return status;
            status = buffer->readCString(*elemName, nullptr);
            if (!status.isOK())
                return status;

            return Status::OK();

        case stdx::to_underlying(BSONType::code):
        case stdx::to_underlying(BSONType::symbol):
        case stdx::to_underlying(BSONType::string):
            status = buffer->readUTF8String(*elemName, nullptr);
            if (!status.isOK())
                return status;
            return Status::OK();

        case stdx::to_underlying(BSONType::binData): {
            int sz;
            if (!buffer->readNumber<int>(&sz))
                return makeError("invalid bson", idElem, *elemName);
            if (sz < 0 || sz == std::numeric_limits<int>::max())
                return makeError("invalid size in bson", idElem, *elemName);
            if (!buffer->skip(1 + sz))
                return makeError("invalid bson", idElem, *elemName);
            return Status::OK();
        }
        case stdx::to_underlying(BSONType::codeWScope):
            *nextState = ValidationState::BeginCodeWScope;
            return Status::OK();
        case stdx::to_underlying(BSONType::object):
        case stdx::to_underlying(BSONType::array):
            *nextState = ValidationState::BeginObj;
            return Status::OK();

        default:
            return makeError("invalid bson type", idElem, *elemName);
    }
}

Status validateBSONIterative(Buffer* buffer) {
    std::vector<ValidationObjectFrame> frames;
    frames.reserve(16);
    ValidationObjectFrame* curr = nullptr;
    ValidationState::State state = ValidationState::BeginObj;

    uint64_t idElemStartPos = 0;  // will become idElem once validated
    BSONElement idElem;

    while (state != ValidationState::Done) {
        switch (state) {
            case ValidationState::BeginObj:
                if (frames.size() > BSONDepth::getMaxAllowableDepth()) {
                    return {ErrorCodes::Overflow,
                            str::stream() << "BSONObj exceeded maximum nested object depth: "
                                          << BSONDepth::getMaxAllowableDepth()};
                }

                frames.push_back(ValidationObjectFrame());
                curr = &frames.back();
                curr->setStartPosition(buffer->position());
                curr->setIsCodeWithScope(false);
                if (!buffer->readNumber<int>(&curr->expectedSize)) {
                    return makeError(
                        "bson size is larger than buffer size", idElem, std::string_view());
                }
                state = ValidationState::WithinObj;
                [[fallthrough]];
            case ValidationState::WithinObj: {
                const bool atTopLevel = frames.size() == 1;
                // check if we've finished validating idElem and are at start of next element.
                if (atTopLevel && idElemStartPos) {
                    idElem = BSONElement(buffer->getBasePtr() + idElemStartPos);
                    buffer->setIdElem(idElem);
                    idElemStartPos = 0;
                }

                const uint64_t elemStartPos = buffer->position();
                ValidationState::State nextState = state;
                std::string_view elemName;
                Status status = validateElementInfo(buffer, &nextState, idElem, &elemName);
                if (!status.isOK())
                    return status;

                // we've already validated that fieldname is safe to access as long as we aren't
                // at the end of the object, since EOO doesn't have a fieldname.
                if (nextState != ValidationState::EndObj && idElem.eoo() && atTopLevel) {
                    if (elemName == "_id") {
                        idElemStartPos = elemStartPos;
                    }
                }

                state = nextState;
                break;
            }
            case ValidationState::EndObj: {
                int actualLength = buffer->position() - curr->startPosition();
                if (actualLength != curr->expectedSize) {
                    return makeError(
                        "bson length doesn't match what we found", idElem, std::string_view());
                }
                frames.pop_back();
                if (frames.empty()) {
                    state = ValidationState::Done;
                } else {
                    curr = &frames.back();
                    if (curr->isCodeWithScope())
                        state = ValidationState::EndCodeWScope;
                    else
                        state = ValidationState::WithinObj;
                }
                break;
            }
            case ValidationState::BeginCodeWScope: {
                frames.push_back(ValidationObjectFrame());
                curr = &frames.back();
                curr->setStartPosition(buffer->position());
                curr->setIsCodeWithScope(true);
                if (!buffer->readNumber<int>(&curr->expectedSize))
                    return makeError("invalid bson CodeWScope size", idElem, std::string_view());
                Status status = buffer->readUTF8String(std::string_view(), nullptr);
                if (!status.isOK())
                    return status;
                state = ValidationState::BeginObj;
                break;
            }
            case ValidationState::EndCodeWScope: {
                int actualLength = buffer->position() - curr->startPosition();
                if (actualLength != curr->expectedSize) {
                    return makeError("bson length for CodeWScope doesn't match what we found",
                                     idElem,
                                     std::string_view());
                }
                frames.pop_back();
                if (frames.empty())
                    return makeError("unnested CodeWScope", idElem, std::string_view());
                curr = &frames.back();
                state = ValidationState::WithinObj;
                break;
            }
            case ValidationState::Done:
                MONGO_UNREACHABLE;
        }
    }

    return Status::OK();
}

}  // namespace

namespace fuzzerOnly {
Status validateBSON(const char* originalBuffer, uint64_t maxLength) {
    if (maxLength < 5) {
        return Status(ErrorCodes::InvalidBSON, "bson data has to be at least 5 bytes");
    }

    Buffer buf(originalBuffer, maxLength);
    return validateBSONIterative(&buf);
}
}  // namespace fuzzerOnly
}  // namespace mongo
