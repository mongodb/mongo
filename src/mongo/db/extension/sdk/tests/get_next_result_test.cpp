// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/get_next_result.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>


namespace mongo::extension::sdk {

namespace {


/**
 * GuardedByteContainer is a guard around a MongoExtensionByteContainer.
 * It is a helper used to ensure a ::MongoExtensionByteContainer is correctly cleaned up if its
 * contents are not released before the object goes out of scope. It is primarily used in our unit
 * tests, but we could leverage it in our production code where necessary.
 */
class GuardedByteContainer {
public:
    GuardedByteContainer(::MongoExtensionByteContainer&& byteContainer)
        : _byteContainer(byteContainer) {
        byteContainer = createEmptyByteContainer();
    }

    ~GuardedByteContainer() {
        if (hasByteBuf()) {
            [[maybe_unused]] ExtensionByteBufHandle handle{_byteContainer.bytes.buf};
        }
    }

    GuardedByteContainer(const GuardedByteContainer& other) = delete;
    GuardedByteContainer& operator=(const GuardedByteContainer& other) = delete;

    GuardedByteContainer(GuardedByteContainer&& other)
        : _byteContainer(std::move(other._byteContainer)) {
        other._byteContainer = createEmptyByteContainer();
    }

    GuardedByteContainer& operator=(GuardedByteContainer&& other) {
        _byteContainer = std::move(other._byteContainer);
        other._byteContainer = createEmptyByteContainer();
        return *this;
    }

    bool hasView() const {
        return _byteContainer.type == ::MongoExtensionByteContainerType::kByteView;
    }

    bool hasByteBuf() const {
        return _byteContainer.type == ::MongoExtensionByteContainerType::kByteBuf;
    }

    /**
     * Transfers owneship of the byte buf to the caller. Container must hold a byte buf!
     */
    ExtensionByteBufHandle releaseByteBuf() {
        tassert(11357809, "GuardedByteContainer did not have a ByteBuf", hasByteBuf());
        ExtensionByteBufHandle handle{_byteContainer.bytes.buf};
        _byteContainer = createEmptyByteContainer();  // Reset the state of the byte container to
                                                      // an empty state.
        return handle;
    }

    /**
     * Return the contained byte view.
     */
    MongoExtensionByteView getByteView() const {
        return _byteContainer.bytes.view;
    }

private:
    ::MongoExtensionByteContainer _byteContainer;
};

TEST(ExtensionBSONObjTest, makeAsByteBufSucceeds) {
    auto originalBSONObj = BSON("meow" << "santiago");
    auto extensionBSONObj = ExtensionBSONObj::makeAsByteBuf(originalBSONObj);
    // clear out the original BSONObj, ensure our ExtensionBSONObj's contents remain valid.
    originalBSONObj = BSONObj();
    auto expectedResultBSON = BSON("meow" << "santiago");
    ASSERT_BSONOBJ_EQ(expectedResultBSON, extensionBSONObj.getUnownedBSONObj());
}

TEST(ExtensionBSONObjTest, makeAsByteViewSucceeds) {
    auto expectedResultBSON = BSON("meow" << "santiago");
    auto extensionBSONObj = ExtensionBSONObj::makeAsByteView(expectedResultBSON);
    ASSERT_BSONOBJ_EQ(expectedResultBSON, extensionBSONObj.getUnownedBSONObj());
}

TEST(ExtensionBSONObjTest, makeFromByteContainerByteBufRoundtrip) {
    auto originalBSONObj = BSON("meow" << "santiago");
    auto extensionBSONObj = ExtensionBSONObj::makeAsByteBuf(originalBSONObj);
    // Clear out the original BSONObj, ensure our ExtensionBSONObj's contents remain valid.
    originalBSONObj = BSONObj();
    ::MongoExtensionByteContainer byteContainer = createEmptyByteContainer();
    // Transfer contents to byte container.
    extensionBSONObj.toByteContainer(byteContainer);
    // Use a guarded byte container to prevent memory leaks.
    GuardedByteContainer guardedByteContainer(std::move(byteContainer));

    // Ensure contents are empty!
    extensionBSONObj = ExtensionBSONObj{};
    ASSERT_BSONOBJ_EQ(BSONObj(), extensionBSONObj.getUnownedBSONObj());

    // Make sure GuardedByteContainer clears out original byte container.
    ASSERT_EQ(::MongoExtensionByteContainerType::kByteView,
              byteContainer.type);               // NOLINT(bugprone-use-after-move)
    ASSERT_EQ(0, byteContainer.bytes.view.len);  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(guardedByteContainer.hasByteBuf());

    auto byteBufHandle = guardedByteContainer.releaseByteBuf();
    ::MongoExtensionByteContainer newContainer{.type = ::MongoExtensionByteContainerType::kByteBuf};
    newContainer.bytes.buf = byteBufHandle.release();
    extensionBSONObj = ExtensionBSONObj::makeFromByteContainer(newContainer);
    ASSERT_BSONOBJ_EQ(BSON("meow" << "santiago"), extensionBSONObj.getUnownedBSONObj());
}

TEST(ExtensionBSONObjTest, makeFromByteContainerByteViewRoundtrip) {
    auto originalBSONObj = BSON("meow" << "santiago");
    auto extensionBSONObj = ExtensionBSONObj::makeAsByteView(originalBSONObj);

    ::MongoExtensionByteContainer byteContainer = createEmptyByteContainer();
    // Transfer contents to byte container.
    extensionBSONObj.toByteContainer(byteContainer);
    ASSERT_EQ(::MongoExtensionByteContainerType::kByteView, byteContainer.type);
    // Use a guarded byte container to prevent memory leaks.
    GuardedByteContainer guardedByteContainer(std::move(byteContainer));

    // Ensure contents are empty!
    extensionBSONObj = ExtensionBSONObj{};
    ASSERT_BSONOBJ_EQ(BSONObj(), extensionBSONObj.getUnownedBSONObj());

    // Make sure GuardedByteContainer clears out original byte container.
    ASSERT_EQ(::MongoExtensionByteContainerType::kByteView,
              byteContainer.type);               // NOLINT(bugprone-use-after-move)
    ASSERT_EQ(0, byteContainer.bytes.view.len);  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(guardedByteContainer.hasView());

    ::MongoExtensionByteContainer newContainer{.type =
                                                   ::MongoExtensionByteContainerType::kByteView};
    newContainer.bytes.view = guardedByteContainer.getByteView();
    extensionBSONObj = ExtensionBSONObj::makeFromByteContainer(newContainer);
    ASSERT_BSONOBJ_EQ(BSON("meow" << "santiago"), extensionBSONObj.getUnownedBSONObj());
}

TEST(ExtensionGetNextResultTest, isEmptyByteContainerTreatsDanglingEmptyViewAsEmpty) {
    ::MongoExtensionByteContainer container;
    container.type = ::MongoExtensionByteContainerType::kByteView;
    // Non-null dangling pointer (Rust empty Vec<u8> sentinel) with len 0.
    container.bytes.view = ::MongoExtensionByteView{reinterpret_cast<const uint8_t*>(0x1), 0};
    ASSERT_TRUE(ExtensionGetNextResult::isEmptyByteContainer(container));
}

TEST(ExtensionGetNextResultTest, isEmptyByteContainerNullEmptyView) {
    auto container = createEmptyByteContainer();
    ASSERT_TRUE(ExtensionGetNextResult::isEmptyByteContainer(container));
}

TEST(ExtensionGetNextResultTest, isEmptyByteContainerTreatsNullDataWithBogusLengthAsEmpty) {
    ::MongoExtensionByteContainer container;
    container.type = ::MongoExtensionByteContainerType::kByteView;
    // Null data with a non-zero length is never valid BSON; treat as empty so the host
    // does not dereference null trying to build a BSONObj from it.
    container.bytes.view = ::MongoExtensionByteView{nullptr, 5};
    ASSERT_TRUE(ExtensionGetNextResult::isEmptyByteContainer(container));
}

TEST(ExtensionGetNextResultTest, makeFromApiResultAdvancedEmptyDocumentThrows) {
    ::MongoExtensionGetNextResult apiResult;
    apiResult.code = ::MongoExtensionGetNextResultCode::kAdvanced;
    apiResult.resultDocument.type = ::MongoExtensionByteContainerType::kByteView;
    apiResult.resultDocument.bytes.view =
        ::MongoExtensionByteView{reinterpret_cast<const uint8_t*>(0x1), 0};
    apiResult.resultMetadata = createEmptyByteContainer();

    ASSERT_THROWS_CODE(ExtensionGetNextResult::makeFromApiResult(apiResult),
                       DBException,
                       ErrorCodes::ExtensionSerializationError);
}

TEST(ExtensionGetNextResultTest, makeFromApiResultAdvancedDanglingEmptyMetadataSkipsMetadata) {
    auto doc = BSON("meow" << "santiago");
    ::MongoExtensionGetNextResult apiResult;
    apiResult.code = ::MongoExtensionGetNextResultCode::kAdvanced;
    apiResult.resultDocument.type = ::MongoExtensionByteContainerType::kByteView;
    apiResult.resultDocument.bytes.view = objAsByteView(doc);
    apiResult.resultMetadata.type = ::MongoExtensionByteContainerType::kByteView;
    apiResult.resultMetadata.bytes.view =
        ::MongoExtensionByteView{reinterpret_cast<const uint8_t*>(0x1), 0};

    auto result = ExtensionGetNextResult::makeFromApiResult(apiResult);
    ASSERT_EQ(GetNextCode::kAdvanced, result.code);
    ASSERT_TRUE(result.resultDocument.has_value());
    ASSERT_FALSE(result.resultMetadata.has_value());  // dangling-empty metadata skipped
}

TEST(ExtensionGetNextResultTest, makeFromApiResultAdvancedNullDataWithBogusLengthThrows) {
    ::MongoExtensionGetNextResult apiResult;
    apiResult.code = ::MongoExtensionGetNextResultCode::kAdvanced;
    apiResult.resultDocument.type = ::MongoExtensionByteContainerType::kByteView;
    // Null data with non-zero length; isEmptyByteContainer's `data == nullptr` branch must
    // reject it before bsonObjFromByteView() dereferences null to read the BSON header.
    apiResult.resultDocument.bytes.view = ::MongoExtensionByteView{nullptr, 5};
    apiResult.resultMetadata = createEmptyByteContainer();

    ASSERT_THROWS_CODE(ExtensionGetNextResult::makeFromApiResult(apiResult),
                       DBException,
                       ErrorCodes::ExtensionSerializationError);
}

TEST(ExtensionGetNextResultTest, makeFromApiResultPauseReclaimsTransferredDocumentBuf) {
    auto doc = BSON("meow" << "santiago");
    ::MongoExtensionGetNextResult apiResult = createDefaultExtensionGetNext();
    apiResult.code = ::MongoExtensionGetNextResultCode::kPauseExecution;
    auto byteBufObj = ExtensionBSONObj::makeAsByteBuf(doc);
    byteBufObj.toByteContainer(apiResult.resultDocument);

    auto result = ExtensionGetNextResult::makeFromApiResult(apiResult);
    ASSERT_EQ(GetNextCode::kPauseExecution, result.code);
    ASSERT_FALSE(result.resultDocument.has_value());
    ASSERT_FALSE(result.resultMetadata.has_value());
}

TEST(ExtensionGetNextResultTest, makeFromApiResultEOFReclaimsTransferredMetadataBuf) {
    auto doc = BSON("meow" << "santiago");
    ::MongoExtensionGetNextResult apiResult = createDefaultExtensionGetNext();
    apiResult.code = ::MongoExtensionGetNextResultCode::kEOF;
    auto byteBufObj = ExtensionBSONObj::makeAsByteBuf(doc);
    byteBufObj.toByteContainer(apiResult.resultMetadata);

    auto result = ExtensionGetNextResult::makeFromApiResult(apiResult);
    ASSERT_EQ(GetNextCode::kEOF, result.code);
    ASSERT_FALSE(result.resultDocument.has_value());
    ASSERT_FALSE(result.resultMetadata.has_value());
}
}  // namespace
}  // namespace mongo::extension::sdk
