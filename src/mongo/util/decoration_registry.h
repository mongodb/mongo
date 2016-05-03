/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <type_traits>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/decoration_container.h"

namespace mongo {

/**
 * Registry of decorations.
 *
 * A decoration registry corresponds to the "type" of a DecorationContainer.  For example, if
 * you have two registries, r1 and r2, a DecorationContainer constructed from r1 has instances
 * the decorations declared on r1, and a DecorationContainer constructed from r2 has instances
 * of the decorations declared on r2.
 */
class DecorationRegistry {
    MONGO_DISALLOW_COPYING(DecorationRegistry);

public:
    DecorationRegistry() = default;

    /**
     * Declares a decoration of type T, constructed with T's default constructor, and
     * returns a descriptor for accessing that decoration.
     *
     * NOTE: T's destructor must not throw exceptions.
     */
    template <typename T>
    DecorationContainer::DecorationDescriptorWithType<T> declareDecoration() {
        static_assert(std::is_nothrow_destructible<T>::value,
                      "Decorations must be nothrow destructible");
        return DecorationContainer::DecorationDescriptorWithType<T>(std::move(declareDecoration(
            sizeof(T), std::alignment_of<T>::value, &constructAt<T>, &destructAt<T>)));
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
    void construct(DecorationContainer* decorable) const;

    /**
     * Destroys the decorations declared in this registry on the given instance of "decorable".
     *
     * Called by the DecorationContainer destructor.  Do not call directly.
     */
    void destruct(DecorationContainer* decorable) const;

private:
    /**
     * Function that constructs (initializes) a single instance of a decoration.
     */
    using DecorationConstructorFn = stdx::function<void(void*)>;

    /**
     * Function that destructs (deinitializes) a single instance of a decoration.
     */
    using DecorationDestructorFn = stdx::function<void(void*)>;

    struct DecorationInfo {
        DecorationInfo() {}
        DecorationInfo(DecorationContainer::DecorationDescriptor descriptor,
                       DecorationConstructorFn constructor,
                       DecorationDestructorFn destructor);

        DecorationContainer::DecorationDescriptor descriptor;
        DecorationConstructorFn constructor;
        DecorationDestructorFn destructor;
    };

    using DecorationInfoVector = std::vector<DecorationInfo>;

    template <typename T>
    static void constructAt(void* location) {
        new (location) T();
    }

    template <typename T>
    static void destructAt(void* location) {
        static_cast<T*>(location)->~T();
    }

    /**
     * Declares a decoration with given "constructor" and "destructor" functions,
     * of "sizeBytes" bytes.
     *
     * NOTE: "destructor" must not throw exceptions.
     */
    DecorationContainer::DecorationDescriptor declareDecoration(size_t sizeBytes,
                                                                size_t alignBytes,
                                                                DecorationConstructorFn constructor,
                                                                DecorationDestructorFn destructor);

    DecorationInfoVector _decorationInfo;
    size_t _totalSizeBytes{0};
};

}  // namespace mongo
