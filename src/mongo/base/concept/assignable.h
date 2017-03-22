#pragma once

#include "mongo/base/concept/copy_assignable.h"
#include "mongo/base/concept/copy_constructible.h"

namespace mongo {
namespace concept {
/*!
 * The Assignable concept models a type which can be copy assigned and copy constructed.
 */
struct Assignable : CopyConstructible, CopyAssignable {};
}  // namespace concept
}  // namespace mongo
