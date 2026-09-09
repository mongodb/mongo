// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"

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
        return ExtensionBSONObj(ExtensionByteBufHandle(new ByteBuf(bsonObj)));
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
                _bson = bsonObjFromByteView(tmpHandle->getByteView());
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
            _bson = bsonObjFromByteView(bufHandle->getByteView());
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
 * ExtensionGetNextResult contains methods to set the state of the ExtensionGetNextResult to
 * reflect that of an advanced, paused execution, or eof state. Wrapper for a getNext() result
 * that maps to an ExtensionGetNextResult.
 */
struct ExtensionGetNextResult {
    GetNextCode code{GetNextCode::kEOF};
    boost::optional<ExtensionBSONObj> resultDocument{boost::none};
    boost::optional<ExtensionBSONObj> resultMetadata{boost::none};

    // Make an "advanced" ExtensionGetNextResult with the provided result document.
    static ExtensionGetNextResult advanced(ExtensionBSONObj&& extDocumentBsonObj) {
        return {.code = GetNextCode::kAdvanced, .resultDocument = std::move(extDocumentBsonObj)};
    }

    // Make an "advanced" ExtensionGetNextResult with the provided result document and metadata.
    static ExtensionGetNextResult advanced(ExtensionBSONObj&& extDocumentBsonObj,
                                           ExtensionBSONObj&& extMetadataBsonObj) {
        return {.code = GetNextCode::kAdvanced,
                .resultDocument = std::move(extDocumentBsonObj),
                .resultMetadata = std::move(extMetadataBsonObj)};
    }

    static ExtensionGetNextResult pauseExecution() {
        return {.code = GetNextCode::kPauseExecution};
    }

    static ExtensionGetNextResult eof() {
        return {.code = GetNextCode::kEOF};
    }

    /*
     * Create an ExtensionGetNextResult from the provided ::MongoExtensionGetNextResult.
     * The function requires resultDocument and always includes it in the returned result.
     * resultMetadata is optional — if null, the returned result contains only the document.
     */
    static ExtensionGetNextResult makeAdvancedFromApiResult(
        ::MongoExtensionGetNextResult& apiResult) {
        if (isEmptyByteContainer(apiResult.resultDocument)) {
            // An Advanced result must carry a valid result document; an empty/dangling byte view
            // (e.g. Rust Vec::as_ptr() on an empty Vec) is a contract violation. Surface it as a
            // query-level error instead of building a BSONObj from it.
            uasserted(ErrorCodes::ExtensionSerializationError,
                      "Extension returned kAdvanced with an empty result document");
        }

        auto doc = ExtensionBSONObj::makeFromByteContainer(apiResult.resultDocument);

        // send back metadata only if present
        return isEmptyByteContainer(apiResult.resultMetadata)
            ? advanced(std::move(doc))
            : advanced(std::move(doc),
                       ExtensionBSONObj::makeFromByteContainer(apiResult.resultMetadata));
    };

    // Whether the container has no bytes that can be parsed as a BSON document. In addition to
    // genuinely empty containers, this also returns true for invalid byte views (null data with a
    // non-zero length, or a non-null pointer with len 0) that must not be dereferenced.
    static bool isEmptyByteContainer(const ::MongoExtensionByteContainer& container) {
        switch (container.type) {
            case MongoExtensionByteContainerType::kByteView:
                return container.bytes.view.data == nullptr || container.bytes.view.len == 0;
            case MongoExtensionByteContainerType::kByteBuf:
                return container.bytes.buf == nullptr;
            default:
                MONGO_UNREACHABLE_TASSERT(11390601);
        }
    }

    /**
     * Instantiates a ExtensionGetNextResult from the provided ::MongoExtensionGetNextResult.
     * Sets the code and results of the ExtensionGetNextResult accordingly. If the
     * MongoExtensionGetNextResult struct has an invalid code, asserts in that case.
     */
    static ExtensionGetNextResult makeFromApiResult(::MongoExtensionGetNextResult& apiResult) {
        // get_next() transfers ownership of any kByteBuf to the host regardless of the result
        // code, but only the kAdvanced path consumes the transferred bytes. Reclaim them on every
        // other path (including any assertions) so a misbehaving extension cannot leak a buffer.
        ScopeGuard reclaimTransferredBytes([&] {
            destroyTransferredBytes(apiResult.resultDocument);
            destroyTransferredBytes(apiResult.resultMetadata);
        });

        ExtensionGetNextResult result;
        switch (apiResult.code) {
            case ::MongoExtensionGetNextResultCode::kAdvanced: {
                result = ExtensionGetNextResult::makeAdvancedFromApiResult(apiResult);
                reclaimTransferredBytes.dismiss();
                break;
            }
            case ::MongoExtensionGetNextResultCode::kPauseExecution:
            case ::MongoExtensionGetNextResultCode::kEOF: {
                result = (apiResult.code == ::MongoExtensionGetNextResultCode::kPauseExecution)
                    ? ExtensionGetNextResult::pauseExecution()
                    : ExtensionGetNextResult::eof();
                break;
            }
            default:
                tasserted(ErrorCodes::ExtensionError,
                          str::stream()
                              << "Invalid MongoExtensionGetNextResultCode: " << apiResult.code);
        }
        return result;
    }

    /**
     * Populates a ::MongoExtensionGetNextResult struct with this ExtensionGetNextResult's code
     * and internal results. Transfers ownership of any resources out of this instance and into
     * the apiResult. Asserts that we were provided with a valid getNextResult code. Asserts if
     * the ExtensionGetNextResult doesn't have a value for the result when it's expected or does
     * have a value for a result when it's not expected.
     */
    void toApiResult(::MongoExtensionGetNextResult& apiResult) {
        switch (code) {
            case GetNextCode::kAdvanced: {
                apiResult.code = ::MongoExtensionGetNextResultCode::kAdvanced;
                _toAdvancedApiResult(apiResult);
                break;
            }
            case GetNextCode::kPauseExecution: {
                tassert(ErrorCodes::ExtensionError,
                        str::stream()
                            << "If the ExtensionGetNextResult code is kPauseExecution, then "
                               "there are currently no results to return so "
                               "ExtensionGetNextResult shouldn't have a result. In this case, "
                               "the following result was returned: "
                            << resultDocument->getUnownedBSONObj(),
                        !resultDocument.has_value());
                apiResult.code = ::MongoExtensionGetNextResultCode::kPauseExecution;
                apiResult.resultDocument = createEmptyByteContainer();
                apiResult.resultMetadata = createEmptyByteContainer();
                break;
            }
            case GetNextCode::kEOF: {
                tassert(ErrorCodes::ExtensionError,
                        str::stream()
                            << "If the ExtensionGetNextResult code is kEOF, then there are no "
                               "results to return so ExtensionGetNextResult shouldn't have a "
                               "result. In this case, the following result was returned: "
                            << resultDocument->getUnownedBSONObj(),
                        !resultDocument.has_value());
                apiResult.code = ::MongoExtensionGetNextResultCode::kEOF;
                apiResult.resultDocument = createEmptyByteContainer();
                apiResult.resultMetadata = createEmptyByteContainer();
                break;
            }
            default:
                tasserted(10956804,
                          str::stream() << "Invalid GetNextCode: " << static_cast<int>(code));
        }
    }

private:
    // Destroys a kByteBuf whose ownership was transferred to the host by an extension when the
    // current path does not consume it. kByteView memory is owned by the extension and must not
    // be freed here.
    static void destroyTransferredBytes(::MongoExtensionByteContainer& container) {
        if (container.type == MongoExtensionByteContainerType::kByteBuf && container.bytes.buf) {
            ExtensionByteBufHandle{container.bytes.buf};
        }
    }

    // Internal helper for populating an output ::MongoExtensionGetNextResult.
    void _toAdvancedApiResult(::MongoExtensionGetNextResult& outputResult) {
        tassert(ErrorCodes::ExtensionError,
                "If the ExtensionGetNextResult code is kAdvanced, then ExtensionGetNextResult "
                "should have a result to return.",
                resultDocument.has_value());
        resultDocument->toByteContainer(outputResult.resultDocument);
        if (resultMetadata.has_value()) {
            resultMetadata->toByteContainer(outputResult.resultMetadata);
        } else {
            outputResult.resultMetadata = createEmptyByteContainer();
        }
    }
};

inline ::MongoExtensionGetNextResult createDefaultExtensionGetNext() {
    return {.code = ::MongoExtensionGetNextResultCode::kEOF,
            .resultDocument = createEmptyByteContainer(),
            .resultMetadata = createEmptyByteContainer()};
}
}  // namespace mongo::extension
