#pragma once

#include "mongo/base/concept/assignable.h"
#include "mongo/base/concept/constructible.h"
#include "mongo/base/concept/unique_ptr.h"

namespace mongo {
namespace concept {
/*!
 * Objects conforming to the `CloneFactory` concept are function-like constructs which return
 * objects that are dynamically allocated copies of their inputs.
 * These copies can be made without knowing the actual dynamic type.  The `CloneFactory` type itself
 * must be `Assignable`, in that it can be used with automatically generated copy constructors and
 * copy assignment operators.
 */
template <typename T>
struct CloneFactory : Assignable {
    Constructible<UniquePtr<T>> operator()(const T*) const;
};
}  // namespace concept
}  // namespace mongo
