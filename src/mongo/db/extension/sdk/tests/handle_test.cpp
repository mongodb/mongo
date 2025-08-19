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
#include "mongo/db/extension/sdk/handle.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

/**
 * DestroyableAPI is an abstraction that allows an object that crosses the extension API boundary.
 */
struct DestroyableAPI {
    const struct DestroyableVTable* const vtable;
};

/**
 * Virtual function table for MongoExtensionStatus.
 */
struct DestroyableVTable {
    /**
     * Destroy `DestroyableAPI` and free all associated resources.
     */
    void (*destroy)(DestroyableAPI* ptr);
};

/**
 * DestroyableImpl is an implementation for a DestroyableAPI by the extension side of the API.
 */
class DestroyableImpl final : public DestroyableAPI {
public:
    static const DestroyableVTable VTABLE;

    explicit DestroyableImpl() : DestroyableAPI(&VTABLE) {};
    ~DestroyableImpl() {
        ++sDestroyCount;
    };

    static size_t getDestroyCount() {
        return sDestroyCount;
    }

    static void resetDestroyCount() {
        sDestroyCount = 0;
    }

    static void destroy(DestroyableAPI* ptr) {
        delete reinterpret_cast<DestroyableImpl*>(ptr);
    }

private:
    static inline size_t sDestroyCount = 0;
};


const DestroyableVTable DestroyableImpl::VTABLE =
    DestroyableVTable{.destroy = &DestroyableImpl::destroy};

class OwnedDestroyableHandle : public mongo::extension::sdk::OwnedHandle<DestroyableAPI> {
public:
    explicit OwnedDestroyableHandle(DestroyableAPI* ptr)
        : mongo::extension::sdk::OwnedHandle<DestroyableAPI>(ptr) {
        _assertValidVTable();
    }

protected:
    void _assertVTableConstraints(const VTable_t&) const override {}
};

class UnownedDestroyableHandle : public mongo::extension::sdk::UnownedHandle<DestroyableAPI> {
public:
    explicit UnownedDestroyableHandle(DestroyableAPI* ptr)
        : mongo::extension::sdk::UnownedHandle<DestroyableAPI>(ptr) {
        _assertValidVTable();
    }

protected:
    void _assertVTableConstraints(const VTable_t&) const override {}
};


TEST(HandleTest, ownedHandleMoveAndDestroy) {
    DestroyableImpl::resetDestroyCount();
    OwnedDestroyableHandle handle(std::make_unique<DestroyableImpl>().release());
    ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
    handle.assertValid();
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


    // Assign ownership of new pointer to target handle via getMutablePtr;
    {
        // Create invalid handle, which we will assign into via getMutablePtr().
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

DEATH_TEST(HandleTest, ownedHandleAssertValid, "10596403") {
    OwnedDestroyableHandle handle(nullptr);
    handle.assertValid();
}

TEST(HandleTest, unownedHandleMoveAndDestroy) {
    DestroyableImpl::resetDestroyCount();
    {
        auto destroyableImplPtr = std::make_unique<DestroyableImpl>();

        UnownedDestroyableHandle handle(destroyableImplPtr.get());
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        handle.assertValid();
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
            ASSERT_TRUE(handle.isValid());  // NOLINT(bugprone-use-after-move)
        }
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        {
            // Create invalid handle, which we will move our valid handle into.
            UnownedDestroyableHandle targetHandle(nullptr);
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
            ASSERT_FALSE(targetHandle.isValid());
            // Test move assignment.
            targetHandle = std::move(handle);
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

            ASSERT_TRUE(targetHandle.isValid());
            ASSERT_TRUE(handle.isValid());  // NOLINT(bugprone-use-after-move)
            ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);
        }
        ASSERT_EQUALS(DestroyableImpl::getDestroyCount(), 0);

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

DEATH_TEST(HandleTest, ownedHandleConstructorRejectsNullVtable, "10596404") {
    DestroyableAPI obj{nullptr};
    OwnedDestroyableHandle handle(&obj);
}

DEATH_TEST(HandleTest, unownedHandleConstructorRejectsNullVtable, "10596404") {
    DestroyableAPI obj{nullptr};
    OwnedDestroyableHandle handle(&obj);
}

DEATH_TEST(HandleTest, ownedHandleConstructorRejectsNullDestroyPointer, "10930100") {
    DestroyableVTable invalidVtable{.destroy = nullptr};
    DestroyableAPI obj{&invalidVtable};
    OwnedDestroyableHandle handle(&obj);
}
}  // namespace
}  // namespace mongo
