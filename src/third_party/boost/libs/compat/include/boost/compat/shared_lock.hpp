#ifndef BOOST_COMPAT_SHARED_LOCK_HPP_INCLUDED
#define BOOST_COMPAT_SHARED_LOCK_HPP_INCLUDED

// Copyright 2023 Christian Mazakas.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/compat/detail/throw_system_error.hpp>

#include <memory> // std::addressof
#include <mutex> // std::defer_lock_t
#include <system_error> // std::errc

namespace boost {
namespace compat {

template <class Mutex>
class shared_lock;

template <class Mutex>
void swap( shared_lock<Mutex>& x, shared_lock<Mutex>& y ) noexcept;

template <class Mutex>
class shared_lock {
private:
  Mutex* pm_ = nullptr;
  bool owns_ = false;

public:
  using mutex_type = Mutex;

  shared_lock() noexcept = default;

  explicit shared_lock( mutex_type& m ) : pm_( std::addressof( m ) ) { lock(); }

  shared_lock( mutex_type& m, std::defer_lock_t ) noexcept
      : pm_( std::addressof( m ) ) {}

  shared_lock( mutex_type& m, std::try_to_lock_t )
      : pm_( std::addressof( m ) ) {
    try_lock();
  }

  shared_lock( mutex_type& m, std::adopt_lock_t )
      : pm_( std::addressof( m ) ), owns_{ true } {}

  ~shared_lock() {
    if ( owns_ ) {
      unlock();
    }
  }

  shared_lock( const shared_lock& ) = delete;
  shared_lock& operator=( const shared_lock& ) = delete;

  shared_lock( shared_lock&& u ) noexcept {
    pm_ = u.pm_;
    owns_ = u.owns_;

    u.pm_ = nullptr;
    u.owns_ = false;
  }

  shared_lock& operator=( shared_lock&& u ) noexcept {
    shared_lock( std::move( u ) ).swap( *this );
    return *this;
  }

  void lock() {
    if ( !pm_ ) {
      detail::throw_system_error( std::errc::operation_not_permitted );
    }

    if ( owns_lock() ) {
      detail::throw_system_error( std::errc::resource_deadlock_would_occur );
    }

    pm_->lock_shared();
    owns_ = true;
  }

  bool try_lock() {
    if ( !pm_ ) {
      detail::throw_system_error( std::errc::operation_not_permitted );
    }

    if ( owns_lock() ) {
      detail::throw_system_error( std::errc::resource_deadlock_would_occur );
    }

    bool b = pm_->try_lock_shared();
    owns_ = b;
    return b;
  }

  void unlock() {
    if ( !pm_ || !owns_ ) {
      detail::throw_system_error( std::errc::operation_not_permitted );
    }

    pm_->unlock_shared();
    owns_ = false;
  }

  void swap( shared_lock& u ) noexcept {
    std::swap( pm_, u.pm_ );
    std::swap( owns_, u.owns_ );
  }

  mutex_type* release() noexcept {
    mutex_type* pm = pm_;
    pm_ = nullptr;
    owns_ = false;
    return pm;
  }

  mutex_type* mutex() const noexcept { return pm_; }

  bool owns_lock() const noexcept { return owns_; }
  explicit operator bool() const noexcept { return owns_; }
};

template <class Mutex>
void swap( shared_lock<Mutex>& x, shared_lock<Mutex>& y ) noexcept {
  x.swap( y );
}

} // namespace compat
} // namespace boost

#endif // BOOST_COMPAT_SHARED_LOCK_HPP_INCLUDED
