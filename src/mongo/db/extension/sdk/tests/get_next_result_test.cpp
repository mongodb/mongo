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
}  // namespace
}  // namespace mongo::extension::sdk
