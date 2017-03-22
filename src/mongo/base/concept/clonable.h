#pragma once

#include "mongo/base/concept/constructible.h"
#include "mongo/base/concept/unique_ptr.h"

namespace mongo {
namespace concept {
/*!
 * Objects conforming to the Clonable concept can be dynamically copied, using `this->clone()`.
 * The Clonable concept does not specify the return type of the `clone()` function.
 */
struct Clonable {
    /*! Clonable objects must be safe to destroy, by pointer. */
    virtual ~Clonable() noexcept = 0;

    /*! Clonable objects can be cloned without knowing the actual dynamic type. */
    Constructible<UniquePtr<Clonable>> clone() const;
};
}  // namespace concept
}  // namespace mongo
