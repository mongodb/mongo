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
#include "mongo/db/extension/shared/handle/handle.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::extension {

/**
 * DestroyableCAPI is an abstraction that allows an object that crosses the extension API boundary.
 */
struct DestroyableCAPI {
    const struct DestroyableVTable* const vtable;
};

/**
 * Virtual function table for MongoExtensionStatus.
 */
struct DestroyableVTable {
    /**
     * Destroy `DestroyableCAPI` and free all associated resources.
     */
    void (*destroy)(DestroyableCAPI* ptr);
};

/**
 * DestroyableImpl is an implementation for a DestroyableCAPI by the extension side of the API.
 */
class DestroyableImpl final : public DestroyableCAPI {
public:
    static const DestroyableVTable VTABLE;

    explicit DestroyableImpl() : DestroyableCAPI(&VTABLE) {};
    ~DestroyableImpl() {
        ++sDestroyCount;
    };

    static size_t getDestroyCount() {
        return sDestroyCount;
    }

    static void resetDestroyCount() {
        sDestroyCount = 0;
    }

    static void destroy(DestroyableCAPI* ptr) {
        delete reinterpret_cast<DestroyableImpl*>(ptr);
    }

private:
    static inline size_t sDestroyCount = 0;
};


const DestroyableVTable DestroyableImpl::VTABLE =
    DestroyableVTable{.destroy = &DestroyableImpl::destroy};

class DestroyableAPI;

template <>
struct c_api_to_cpp_api<DestroyableCAPI> {
    using CppApi_t = DestroyableAPI;
};

class DestroyableAPI : public VTableAPI<DestroyableCAPI> {
public:
    explicit DestroyableAPI(DestroyableCAPI* ptr) : VTableAPI<DestroyableCAPI>(ptr) {}
    static void assertVTableConstraints(const VTable_t&) {}
};


using OwnedDestroyableHandle = OwnedHandle<DestroyableCAPI>;
using UnownedDestroyableHandle = UnownedHandle<DestroyableCAPI>;

using UnownedConstDestroyableHandle = UnownedHandle<const DestroyableCAPI>;

namespace {
TEST(HandleTest, ownedHandleMoveAndDestroy) {
    DestroyableImpl::resetDestroyCount();
    OwnedDestroyableHandle handle(new DestroyableImpl());
    ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
    handle->assertValid();
    {
        // Create invalid handle, which we will move our valid handle into.
        OwnedDestroyableHandle targetHandle(nullptr);
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        ASSERT_FALSE(targetHandle.isValid());

        targetHandle = std::move(handle);
        // Ensure moved from handle is now invalid, and target handle is valid.
        ASSERT_TRUE(targetHandle.isValid());
        ASSERT_FALSE(handle.isValid());  // NOLINT(bugprone-use-after-move)
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

        // Reassign to invalid handle, should call destroy.
        targetHandle = OwnedDestroyableHandle(nullptr);
        ASSERT_FALSE(targetHandle.isValid());
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 1);
    }

    {
        OwnedDestroyableHandle targetHandle(nullptr);
        auto destroyableImpl = std::make_unique<DestroyableImpl>();
        const auto* destroyableImplPtr = destroyableImpl.get();
        ASSERT_TRUE(destroyableImplPtr != nullptr);
        targetHandle = OwnedDestroyableHandle(destroyableImpl.release());
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 1);
        ASSERT_TRUE(targetHandle.isValid());
        // targetHandle goes out of scope, should call destroy and increment the count.
    }
    ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 2);
}

TEST(HandleTest, ownedHandleMoveAndDestroyConst) {
    DestroyableImpl::resetDestroyCount();
    OwnedDestroyableHandle ownedHandle(new DestroyableImpl());

    UnownedConstDestroyableHandle handle(ownedHandle.get());
    ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
    handle->assertValid();
    {
        // Create invalid handle, which we will move our valid handle into.
        UnownedConstDestroyableHandle targetHandle(nullptr);
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        ASSERT_FALSE(targetHandle.isValid());

        targetHandle = std::move(handle);
        // Ensure moved from handle is now invalid, and target handle is valid.
        ASSERT_TRUE(targetHandle.isValid());
        ASSERT_FALSE(handle.isValid());  // NOLINT(bugprone-use-after-move)
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

        // Reassign to invalid handle, should not call destroy.
        targetHandle = UnownedConstDestroyableHandle(nullptr);
        ASSERT_FALSE(targetHandle.isValid());
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
    }
    ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
}

DEATH_TEST(HandleTestDeathTest, ownedHandleAssertValid, "10596403") {
    OwnedDestroyableHandle handle(nullptr);
    handle->assertValid();
}

TEST(HandleTest, unownedHandleMoveAndDestroy) {
    DestroyableImpl::resetDestroyCount();
    {
        auto destroyableImplPtr = std::make_unique<DestroyableImpl>();

        UnownedDestroyableHandle handle(destroyableImplPtr.get());
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        handle->assertValid();
        {
            // Create invalid handle, which we will copy our valid handle into.
            UnownedDestroyableHandle targetHandle(nullptr);
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
            ASSERT_FALSE(targetHandle.isValid());
            // Test copy assignment.
            targetHandle = handle;
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

            ASSERT_TRUE(targetHandle.isValid());
            ASSERT_TRUE(handle.isValid());
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        }
        // targetHandle went out of scope, ensure we did not call destructor.
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        {
            // Copy constructor
            UnownedDestroyableHandle targetHandle(handle);
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
            ASSERT_TRUE(targetHandle.isValid());
            ASSERT_TRUE(handle.isValid());
        }
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

        {
            // Move constructor.
            UnownedDestroyableHandle targetHandle(std::move(handle));
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
            ASSERT_TRUE(targetHandle.isValid());
            ASSERT_FALSE(handle.isValid());  // NOLINT(bugprone-use-after-move)
        }
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        handle = UnownedDestroyableHandle(destroyableImplPtr.get());
        ASSERT_TRUE(handle.isValid());

        {
            // Create invalid handle, which we will move our valid handle into.
            UnownedDestroyableHandle targetHandle(nullptr);
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
            ASSERT_FALSE(targetHandle.isValid());
            // Test move assignment.
            targetHandle = std::move(handle);
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

            ASSERT_TRUE(targetHandle.isValid());
            ASSERT_FALSE(handle.isValid());  // NOLINT(bugprone-use-after-move)
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        }
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        handle = UnownedDestroyableHandle(destroyableImplPtr.get());
        ASSERT_TRUE(handle.isValid());
        {
            // Create invalid handle, which we will move our valid handle into.
            UnownedDestroyableHandle targetHandle(nullptr);
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
            ASSERT_FALSE(targetHandle.isValid());
            // Test copy assignment.
            targetHandle = handle;
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

            ASSERT_TRUE(targetHandle.isValid());
            ASSERT_TRUE(handle.isValid());
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        }
    }
    // unique_ptr finally goes out of scope, calls destructor.
    ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 1);
}

DEATH_TEST(HandleTestDeathTest, ownedHandleConstructorRejectsNullVtable, "10596404") {
    DestroyableCAPI obj{nullptr};
    OwnedDestroyableHandle handle(&obj);
}

DEATH_TEST(HandleTestDeathTest, unownedHandleConstructorRejectsNullVtable, "10596404") {
    DestroyableCAPI obj{nullptr};
    UnownedDestroyableHandle handle(&obj);
}

DEATH_TEST(HandleTestDeathTest, ownedHandleConstructorRejectsNullDestroyPointer, "10930100") {
    DestroyableVTable invalidVtable{.destroy = nullptr};
    DestroyableCAPI obj{&invalidVtable};
    OwnedDestroyableHandle handle(&obj);
}
}  // namespace
}  // namespace mongo::extension
