// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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

DEATH_TEST(HandleTestDeathTest, ownedHandleConstructorRejectsNullVtable, "517") {
    DestroyableCAPI obj{nullptr};
    OwnedDestroyableHandle handle(&obj);
}

DEATH_TEST(HandleTestDeathTest, unownedHandleConstructorRejectsNullVtable, "517") {
    DestroyableCAPI obj{nullptr};
    UnownedDestroyableHandle handle(&obj);
}

DEATH_TEST(HandleTestDeathTest, ownedHandleConstructorRejectsNullDestroyPointer, "517") {
    DestroyableVTable invalidVtable{.destroy = nullptr};
    DestroyableCAPI obj{&invalidVtable};
    OwnedDestroyableHandle handle(&obj);
}
}  // namespace
}  // namespace mongo::extension
