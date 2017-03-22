#pragma once

namespace mongo {
namespace concept {
/**
 * The CopyAssignable concept models a type which can be copy assigned.
 *
 * The expression: `copyAssignable= copyAssignable` should be valid.
 */
struct CopyAssignable {
    /**
     * The copy assignment operator is required by `CopyAssignable`.
     * NOTE: Copy Assignment is only required on lvalue targets of `CopyAssignable`.
     */
    CopyAssignable& operator=(const CopyAssignable&) &;
};
}  // namespace concept
}  // namespace mongo
