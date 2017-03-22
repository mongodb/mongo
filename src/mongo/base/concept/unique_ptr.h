#pragma once

#include "mongo/base/concept/convertible_to.h"

namespace mongo {
namespace concept {
/**
 * The `UniquePtr` Concept models a movable owning pointer of an object.
 * `std::unique_ptr< T >` is a model of `mongo::concept::UniquePtr< T >`.
 */
template <typename T>
struct UniquePtr {
    /** The `UniquePtr< T >` must retire its pointer to `T` on destruction. */
    ~UniquePtr() noexcept;

    UniquePtr(UniquePtr&& p);
    UniquePtr& operator=(UniquePtr&& p);

    UniquePtr();
    UniquePtr(T* p);

    ConvertibleTo<T*> operator->() const;
    T& operator*() const;

    explicit operator bool() const;

    ConvertibleTo<T*> get() const;

    void reset() noexcept;
    void reset(ConvertibleTo<T*>);
};

/*! A `UniquePtr` object must be equality comparable. */
template <typename T>
bool operator==(const UniquePtr<T>& lhs, const UniquePtr<T>& rhs);

/*! A `UniquePtr` object must be inequality comparable. */
template <typename T>
bool operator!=(const UniquePtr<T>& lhs, const UniquePtr<T>& rhs);
}  // namespace concept
}  // namespace mongo
