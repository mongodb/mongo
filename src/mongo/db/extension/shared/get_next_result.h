/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

/**
 * ExtensionBSONObj is a helper class used in the context of ExtensionGetNextResult. It is
 * responsible for constructing a BSONObj from a provided ::MongoExtensionByteContainer.
 *
 * If the ExtensionBSONObj is constructed with a container that held a byte buf, ownership of the
 * byte buf is transferred to the ExtensionBSONObj instance, and a BSONObj is constructed from its
 * view.
 *
 * If the ExtensionBSONObj is constructed with a container that held a view, the view must be
 * guaranteed to remain stable for the entire lifetime of this object. In the typical extension
 * doGetNext() case, the view is kept stable by the extension until the subsequent doGetNext() call.
 */
class ExtensionBSONObj {
public:
    explicit ExtensionBSONObj() = default;

    // Move-only semantics due to ByteBufHandle.
    ExtensionBSONObj(const ExtensionBSONObj&) = delete;
    ExtensionBSONObj& operator=(const ExtensionBSONObj&) = delete;
    ExtensionBSONObj(ExtensionBSONObj&& other)
        : _holder(std::move(other._holder)), _bson(std::move(other._bson)) {
        other._holder = ::MongoExtensionByteView{nullptr, 0};
    }

    ExtensionBSONObj& operator=(ExtensionBSONObj&& other) {
        _holder = std::move(other._holder);
        other._holder = ::MongoExtensionByteView{nullptr, 0};
        _bson = std::move(other._bson);
        return *this;
    }

    // This returned BSONObj is never owned, since it's created from the _holder.
    // Therefore, if somebody requires an owned copy, they should make it explicitly.
    BSONObj getUnownedBSONObj() const {
        return _bson;
    }

    // This static method is used to instantiate an ExtensionBSONObj from an existing BSONObj,
    // where the BSONObj can't be guaranteed to remain stable for the lifetime of the new
    // ExtensionGetNextResult. In this case, we allocate a new ByteBuf from which we can issue a
    // BSONObj as a view. It is primarily used from our tests.
    static ExtensionBSONObj makeAsByteBuf(const BSONObj& bsonObj) {
        return ExtensionBSONObj(ExtensionByteBufHandle(new VecByteBuf(bsonObj)));
    }

    // This static method is used to instantiate an ExtensionBSONObj from an existing BSONObj,
    // where the BSONObj is guaranteed to remain valid for the lifetime of the object
    // externally. This means, even if the passed BSONObj was owned, the lifetime is not
    // extended by this object. A typical example would be an ExecAggStage that guarantees to keep
    // the last GetNextResult valid until the subsequent getNext() call.
    static ExtensionBSONObj makeAsByteView(const BSONObj& bsonObj) {
        return ExtensionBSONObj(objAsByteView(bsonObj));
    }

    // This static method is used to instantiate an ExtensionBSONObj from a
    // ::MongoExtensionByteContainer.
    static ExtensionBSONObj makeFromByteContainer(::MongoExtensionByteContainer& container) {
        return ExtensionBSONObj(container);
    }

    // Converts this ExtensionBSONObj to a ::MongoExtensionByteContainer, transferring ownership of
    // resources where applicable.
    void toByteContainer(::MongoExtensionByteContainer& outputContainer) {
        _bson = BSONObj();

        std::visit(
            [&](auto&& holder) {
                using T = std::decay_t<decltype(holder)>;

                if constexpr (std::is_same_v<T, ExtensionByteBufHandle>) {
                    if (holder.isValid()) {
                        outputContainer.type = MongoExtensionByteContainerType::kByteBuf;
                        outputContainer.bytes.buf = holder.release();
                    } else {
                        // Treat invalid handle as an empty view.
                        outputContainer.type = MongoExtensionByteContainerType::kByteView;
                        outputContainer.bytes.view = ::MongoExtensionByteView{nullptr, 0};
                    }
                } else if constexpr (std::is_same_v<T, ::MongoExtensionByteView>) {
                    outputContainer.type = MongoExtensionByteContainerType::kByteView;
                    outputContainer.bytes.view = holder;
                } else {
                    tasserted(11357804, "ExtensionBSONObj's holder was invalid");
                }
            },
            _holder);
    }

private:
    explicit ExtensionBSONObj(::MongoExtensionByteContainer& container) {
        switch (container.type) {
            case kByteBuf: {
                // Make ExtensionBSONObj with a provided ExtensionByteBufHandle.
                // ExtensionByteBufHandle was provided by extension, and is owned by the _holder
                // handle we create here.
                // ExtensionByteBufHandle must remain consistent for the same lifetime as the
                // BSONObj.
                auto tmpHandle = ExtensionByteBufHandle{container.bytes.buf};
                _bson = bsonObjFromByteView(tmpHandle.getByteView());
                _holder = std::move(tmpHandle);
            } break;
            case kByteView:
                // Make ExtensionBSONObj with a provided ByteView. ByteView must be guaranteed
                // to be kept consistent/valid until next getNext() is called. This is only used
                // in scenarios in which the extension guarantees to keep view valid for us.
                _holder = container.bytes.view;
                _bson = bsonObjFromByteView(container.bytes.view);
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(11357805);
                break;
        }
    }

    explicit ExtensionBSONObj(::MongoExtensionByteView view)
        : _holder(view), _bson(bsonObjFromByteView(view)) {
        // Make ExtensionBSONObj with a provided ByteView. ByteView must be guaranteed
        // to be kept consistent/valid until next getNext() is called. This is only used in
        // scenarios in which the extension guarantees to keep view valid for us.
    }

    explicit ExtensionBSONObj(ExtensionByteBufHandle bufHandle) {
        if (bufHandle.isValid()) {
            _bson = bsonObjFromByteView(bufHandle.getByteView());
            _holder = std::move(bufHandle);
        }
    }

    std::variant<ExtensionByteBufHandle, ::MongoExtensionByteView> _holder{
        ::MongoExtensionByteView{nullptr, 0}};
    // This BSON is never owned. If anyone needs an owned copy of the BSON, they
    // must create an owned copy themselves.
    BSONObj _bson = BSONObj();
};

/**
 * GetNextCode contains all possible ::MongoExtensionGetNextResultCode values.
 */
enum class GetNextCode { kAdvanced, kEOF, kPauseExecution };

/**
 * GetNextRequestType contains all possible ::MongoExtensionGetNextRequestType values.
 */
enum class GetNextRequestType { kNone, kDocumentOnly, kMetadataOnly, kDocumentAndMetadata };

/**
 * ExtensionGetNextResult contains methods to set the state of the ExtensionGetNextResult to
 * reflect that of an advanced, paused execution, or eof state. Wrapper for a getNext() result
 * that maps to an ExtensionGetNextResult.
 */
struct ExtensionGetNextResult {
    GetNextCode code{GetNextCode::kEOF};
    boost::optional<ExtensionBSONObj> resultDocument{boost::none};
    GetNextRequestType requestType{GetNextRequestType::kDocumentOnly};

    // Converts a MongoExtensionGetNextRequestType into a GetNextRequestType. Static function for
    // testing purposes.
    static inline GetNextRequestType fromApiRequestType(
        ::MongoExtensionGetNextRequestType apiRequestType) {
        switch (apiRequestType) {
            case ::MongoExtensionGetNextRequestType::kNone:
                return GetNextRequestType::kNone;
            case ::MongoExtensionGetNextRequestType::kDocumentOnly:
                return GetNextRequestType::kDocumentOnly;
            case ::MongoExtensionGetNextRequestType::kMetadataOnly:
                return GetNextRequestType::kMetadataOnly;
            case ::MongoExtensionGetNextRequestType::kDocumentAndMetadata:
                return GetNextRequestType::kDocumentAndMetadata;
            default:
                tasserted(11357807,
                          str::stream()
                              << "Invalid ::MongoExtensionGetNextRequestType" << apiRequestType);
        }
    }

    // Converts a GetNextRequestType into a MongoExtensionGetNextRequestType. Static function for
    // testing purposes.
    static inline ::MongoExtensionGetNextRequestType setApiRequestType(
        GetNextRequestType extensionRequestType) {
        switch (extensionRequestType) {
            case GetNextRequestType::kNone:
                return ::MongoExtensionGetNextRequestType::kNone;
            case GetNextRequestType::kDocumentOnly:
                return ::MongoExtensionGetNextRequestType::kDocumentOnly;
            case GetNextRequestType::kMetadataOnly:
                return ::MongoExtensionGetNextRequestType::kMetadataOnly;
            case GetNextRequestType::kDocumentAndMetadata:
                return ::MongoExtensionGetNextRequestType::kDocumentAndMetadata;
            default:
                tasserted(11357808,
                          str::stream() << "Invalid GetNextRequestType: "
                                        << static_cast<int>(extensionRequestType));
        }
    }

    // Make an "advanced" ExtensionGetNextResult with the provided result document.
    static ExtensionGetNextResult advanced(ExtensionBSONObj&& extBsonObj) {
        // TODO SERVER-113905: Update this factory function to accommodate both document and
        // metadata.
        return {.code = GetNextCode::kAdvanced, .resultDocument = std::move(extBsonObj)};
    }

    static ExtensionGetNextResult pauseExecution() {
        return {GetNextCode::kPauseExecution};
    }

    static ExtensionGetNextResult eof() {
        return {GetNextCode::kEOF};
    }

    static ExtensionGetNextResult makeAdvancedFromApiResult(
        ::MongoExtensionGetNextResult& apiResult) {
        switch (apiResult.requestType) {
            case kDocumentOnly:
                return ExtensionGetNextResult::advanced(
                    ExtensionBSONObj::makeFromByteContainer(apiResult.resultDocument));
            default:
                break;
        }
        // TODO SERVER-113905: we only support returning document for now. Later, we should only
        // support returning both document and metadata.
        MONGO_UNREACHABLE_TASSERT(11357803);
    };

    /**
     * Instantiates a ExtensionGetNextResult from the provided ::MongoExtensionGetNextResult. Sets
     * the code and results of the ExtensionGetNextResult accordingly. If the
     * MongoExtensionGetNextResult struct has an invalid code, asserts in that case.
     */
    static ExtensionGetNextResult makeFromApiResult(::MongoExtensionGetNextResult& apiResult) {
        ExtensionGetNextResult result;
        switch (apiResult.code) {
            case ::MongoExtensionGetNextResultCode::kAdvanced: {
                result = ExtensionGetNextResult::makeAdvancedFromApiResult(apiResult);
                break;
            }
            case ::MongoExtensionGetNextResultCode::kPauseExecution:
                result = ExtensionGetNextResult::pauseExecution();
                break;
            case ::MongoExtensionGetNextResultCode::kEOF: {
                result = ExtensionGetNextResult::eof();
                break;
            }
            default:
                tasserted(10956803,
                          str::stream()
                              << "Invalid MongoExtensionGetNextResultCode: " << apiResult.code);
        }
        result.requestType = fromApiRequestType(apiResult.requestType);
        return result;
    }

    /**
     * Populates a ::MongoExtensionGetNextResult struct with this ExtensionGetNextResult's code and
     * internal results. Transfers ownership of any resources out of this instance and into the
     * apiResult. Asserts that we were provided with a valid getNextResult code. Asserts if the
     * ExtensionGetNextResult doesn't have a value for the result when it's expected or
     * does have a value for a result when it's not expected.
     */
    void toApiResult(::MongoExtensionGetNextResult& apiResult) {
        switch (code) {
            case GetNextCode::kAdvanced: {
                apiResult.code = ::MongoExtensionGetNextResultCode::kAdvanced;
                _toAdvancedApiResult(apiResult);
                break;
            }
            case GetNextCode::kPauseExecution: {
                tassert(10956802,
                        str::stream()
                            << "If the ExtensionGetNextResult code is kPauseExecution, then "
                               "there are currently no results to return so "
                               "ExtensionGetNextResult shouldn't have a result. In this case, "
                               "the following result was returned: "
                            << resultDocument->getUnownedBSONObj(),
                        !resultDocument.has_value());
                apiResult.code = ::MongoExtensionGetNextResultCode::kPauseExecution;
                apiResult.resultDocument = createEmptyByteContainer();
                break;
            }
            case GetNextCode::kEOF: {
                tassert(10956805,
                        str::stream()
                            << "If the ExtensionGetNextResult code is kEOF, then there are no "
                               "results to return so ExtensionGetNextResult shouldn't have a "
                               "result. In this case, the following result was returned: "
                            << resultDocument->getUnownedBSONObj(),
                        !resultDocument.has_value());
                apiResult.code = ::MongoExtensionGetNextResultCode::kEOF;
                apiResult.resultDocument = createEmptyByteContainer();
                break;
            }
            default:
                tasserted(10956804,
                          str::stream() << "Invalid GetNextCode: " << static_cast<int>(code));
        }
        apiResult.requestType = setApiRequestType(requestType);
    }

private:
    // Internal helper for populating an output ::MongoExtensionGetNextResult.
    void _toAdvancedApiResult(::MongoExtensionGetNextResult& outputResult) {
        switch (outputResult.requestType) {
            case kDocumentOnly:
                tassert(
                    10956801,
                    "If the ExtensionGetNextResult code is kAdvanced, then ExtensionGetNextResult "
                    "should have a result to return.",
                    resultDocument.has_value());
                resultDocument->toByteContainer(outputResult.resultDocument);
                // TODO SERVER-113905, once we support metadata, we should update these switch
                // statements to support returning both document and metadata.
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(11357801);
                break;
        }
    }
};

inline ::MongoExtensionGetNextResult createDefaultExtensionGetNext(
    ::MongoExtensionGetNextRequestType requestType) {
    return {.code = ::MongoExtensionGetNextResultCode::kEOF,
            .resultDocument = createEmptyByteContainer(),
            .requestType = requestType};
}
}  // namespace mongo::extension
