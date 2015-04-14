//
// detail/impl/service_registry.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_IMPL_SERVICE_REGISTRY_HPP
#define ASIO_DETAIL_IMPL_SERVICE_REGISTRY_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

template <typename Service>
Service& service_registry::use_service()
{
  execution_context::service::key key;
  init_key(key, Service::id);
  factory_type factory = &service_registry::create<Service, execution_context>;
  return *static_cast<Service*>(do_use_service(key, factory, &owner_));
}

template <typename Service>
Service& service_registry::use_service(io_service& owner)
{
  execution_context::service::key key;
  init_key(key, Service::id);
  factory_type factory = &service_registry::create<Service, io_service>;
  return *static_cast<Service*>(do_use_service(key, factory, &owner));
}

template <typename Service>
void service_registry::add_service(Service* new_service)
{
  execution_context::service::key key;
  init_key(key, Service::id);
  return do_add_service(key, new_service);
}

template <typename Service>
bool service_registry::has_service() const
{
  execution_context::service::key key;
  init_key(key, Service::id);
  return do_has_service(key);
}

#if !defined(ASIO_NO_TYPEID)
template <typename Service>
void service_registry::init_key(execution_context::service::key& key,
    const service_id<Service>& /*id*/)
{
  key.type_info_ = &typeid(typeid_wrapper<Service>);
  key.id_ = 0;
}
#endif // !defined(ASIO_NO_TYPEID)

template <typename Service, typename Owner>
execution_context::service* service_registry::create(void* owner)
{
  return new Service(*static_cast<Owner*>(owner));
}

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_IMPL_SERVICE_REGISTRY_HPP
