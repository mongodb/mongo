/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <algorithm>
#include <functional>
#include <iterator>
#include <type_traits>
#include <vector>

#include "mongo/base/static_assert.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/decoration_container.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

/**
 * Registry of decorations.
 *
 * A decoration registry corresponds to the "type" of a DecorationContainer.  For example, if
 * you have two registries, r1 and r2, a DecorationContainer constructed from r1 has instances
 * the decorations declared on r1, and a DecorationContainer constructed from r2 has instances
 * of the decorations declared on r2.
 */
template <typename DecoratedType>
class DecorationRegistry {
    DecorationRegistry(const DecorationRegistry&) = delete;
    DecorationRegistry& operator=(const DecorationRegistry&) = delete;

public:
    DecorationRegistry() = default;

    /**
     * Declares a decoration of type T, constructed with T's default constructor, and
     * returns a descriptor for accessing that decoration.
     *
     * NOTE: T's destructor must not throw exceptions.
     */
    template <typename T>
    auto declareDecoration() {
        MONGO_STATIC_ASSERT_MSG(std::is_nothrow_destructible<T>::value,
                                "Decorations must be nothrow destructible");
        return
            typename DecorationContainer<DecoratedType>::template DecorationDescriptorWithType<T>(
                std::move(declareDecoration(
                    sizeof(T), std::alignment_of<T>::value, &constructAt<T>, &destroyAt<T>)));
    }

    size_t getDecorationBufferSizeBytes() const {
        return _totalSizeBytes;
    }

    /**
     * Constructs the decorations declared in this registry on the given instance of
     * "decorable".
     *
     * Called by the DecorationContainer constructor. Do not call directly.
     */
    void construct(DecorationContainer<DecoratedType>* const container) const {
        using std::cbegin;

        auto iter = cbegin(_decorationInfo);

        auto cleanupFunction = [&iter, container, this ]() noexcept->void {
            using std::crend;
            std::for_each(std::make_reverse_iterator(iter),
                          crend(this->_decorationInfo),
                          [&](auto&& decoration) {
                              decoration.destructor(
                                  container->getDecoration(decoration.descriptor));
                          });
        };

        auto cleanup = makeGuard(std::move(cleanupFunction));

        using std::cend;

        for (; iter != cend(_decorationInfo); ++iter) {
            iter->constructor(container->getDecoration(iter->descriptor));
        }

        cleanup.dismiss();
    }

    /**
     * Destroys the decorations declared in this registry on the given instance of "decorable".
     *
     * Called by the DecorationContainer destructor.  Do not call directly.
     */
    void destroy(DecorationContainer<DecoratedType>* const container) const noexcept try {
        std::for_each(_decorationInfo.rbegin(), _decorationInfo.rend(), [&](auto&& decoration) {
            decoration.destructor(container->getDecoration(decoration.descriptor));
        });
    } catch (...) {
        std::terminate();
    }

private:
    /**
     * Function that constructs (initializes) a single instance of a decoration.
     */
    using DecorationConstructorFn = void (*)(void*);

    /**
     * Function that destroys (deinitializes) a single instance of a decoration.
     */
    using DecorationDestructorFn = void (*)(void*);

    struct DecorationInfo {
        DecorationInfo() {}
        DecorationInfo(
            typename DecorationContainer<DecoratedType>::DecorationDescriptor inDescriptor,
            DecorationConstructorFn inConstructor,
            DecorationDestructorFn inDestructor)
            : descriptor(std::move(inDescriptor)),
              constructor(std::move(inConstructor)),
              destructor(std::move(inDestructor)) {}

        typename DecorationContainer<DecoratedType>::DecorationDescriptor descriptor;
        DecorationConstructorFn constructor;
        DecorationDestructorFn destructor;
    };

    using DecorationInfoVector = std::vector<DecorationInfo>;

    template <typename T>
    static void constructAt(void* location) {
        new (location) T();
    }

    template <typename T>
    static void destroyAt(void* location) {
        static_cast<T*>(location)->~T();
    }

    /**
     * Declares a decoration with given "constructor" and "destructor" functions,
     * of "sizeBytes" bytes.
     *
     * NOTE: "destructor" must not throw exceptions.
     */
    typename DecorationContainer<DecoratedType>::DecorationDescriptor declareDecoration(
        const size_t sizeBytes,
        const size_t alignBytes,
        const DecorationConstructorFn constructor,
        const DecorationDestructorFn destructor) {
        const size_t misalignment = _totalSizeBytes % alignBytes;
        if (misalignment) {
            _totalSizeBytes += alignBytes - misalignment;
        }
        typename DecorationContainer<DecoratedType>::DecorationDescriptor result(_totalSizeBytes);
        _decorationInfo.push_back(DecorationInfo(result, constructor, destructor));
        _totalSizeBytes += sizeBytes;
        return result;
    }

    DecorationInfoVector _decorationInfo;
    size_t _totalSizeBytes{sizeof(void*)};
};

}  // namespace mongo
