//
// boost/process/v2/bind_launcher.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_BIND_LAUNCHER_HPP
#define BOOST_PROCESS_V2_BIND_LAUNCHER_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/default_launcher.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{

template<std::size_t ... Idx>
struct index_sequence { };

template<std::size_t Size, typename T> 
struct make_index_sequence_impl;

template<std::size_t Size, std::size_t ... Idx> 
struct make_index_sequence_impl<Size, index_sequence<Idx...>>
{
    constexpr make_index_sequence_impl() {}
    using type = typename make_index_sequence_impl<Size - 1u, index_sequence<Size - 1u, Idx...>>::type;
};

template<std::size_t ... Idx> 
struct make_index_sequence_impl<0u, index_sequence<Idx...>>
{
    constexpr make_index_sequence_impl() {}
    using type = index_sequence<Idx...>;
};


template<std::size_t Cnt>
struct make_index_sequence
{
    using type = typename make_index_sequence_impl<Cnt, index_sequence<>>::type;
};

template<std::size_t Cnt>
using make_index_sequence_t = typename make_index_sequence<Cnt>::type;

}

/** @brief  Utility class to bind initializers to a launcher
 * @tparam Launcher The inner launcher to be used
 * @tparam ...Init The initializers to be prepended.
 * 
 * This can be used when multiple processes shared some settings, 
 * e.g. 
 * 
 */
template<typename Launcher, typename ... Init>
struct bound_launcher
{
    template<typename Launcher_, typename ... Init_>
    bound_launcher(Launcher_ && l, Init_ && ... init) : 
        launcher_(std::forward<Launcher_>(l)), init_(std::forward<Init_>(init)...) 
    {
    }

    template<typename ExecutionContext, typename Args, typename ... Inits>
    auto operator()(ExecutionContext & context,
                    const typename std::enable_if<std::is_convertible<
                            ExecutionContext&, net::execution_context&>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits) -> basic_process<typename ExecutionContext::executor_type>
    {
        return invoke(detail::make_index_sequence_t<sizeof...(Init)>{},
                      context, 
                      executable,  
                      std::forward<Args>(args), 
                      std::forward<Inits>(inits)...);
    }


    template<typename ExecutionContext, typename Args, typename ... Inits>
    auto operator()(ExecutionContext & context,
                    error_code & ec,
                    const typename std::enable_if<std::is_convertible<
                            ExecutionContext&, net::execution_context&>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<typename ExecutionContext::executor_type>
    {
        return invoke(detail::make_index_sequence_t<sizeof...(Init)>{},
                      context, ec,
                      executable,  
                      std::forward<Args>(args), 
                      std::forward<Inits>(inits)...);
    }

    template<typename Executor, typename Args, typename ... Inits>
    auto operator()(Executor exec,
                    const typename std::enable_if<
                            net::execution::is_executor<Executor>::value ||
                            net::is_executor<Executor>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<Executor>
    {
        return invoke(detail::make_index_sequence_t<sizeof...(Init)>{},
                      std::move(exec), 
                      executable,  
                      std::forward<Args>(args), 
                      std::forward<Inits>(inits)...);
    }

    template<typename Executor, typename Args, typename ... Inits>
    auto operator()(Executor exec,
                    error_code & ec,
                    const typename std::enable_if<
                            net::execution::is_executor<Executor>::value ||
                            net::is_executor<Executor>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<Executor>
    {
        return invoke(detail::make_index_sequence_t<sizeof...(Init)>{},
                      std::move(exec), ec,
                      executable,  
                      std::forward<Args>(args), 
                      std::forward<Inits>(inits)...);
    }

  private:
    template<std::size_t ... Idx, typename ExecutionContext, typename Args, typename ... Inits>
    auto invoke(detail::index_sequence<Idx...>, 
                ExecutionContext & context,
                const typename std::enable_if<std::is_convertible<
                        ExecutionContext&, net::execution_context&>::value,
                        filesystem::path >::type & executable,
                Args && args,
                Inits && ... inits) -> basic_process<typename ExecutionContext::executor_type>
    {
        return launcher_(context, 
                         executable,  
                         std::forward<Args>(args), 
                         std::get<Idx>(init_)...,
                         std::forward<Inits>(inits)...);
    }


    template<std::size_t ... Idx, typename ExecutionContext, typename Args, typename ... Inits>
    auto invoke(detail::index_sequence<Idx...>,
                ExecutionContext & context,
                error_code & ec,
                const typename std::enable_if<std::is_convertible<
                        ExecutionContext&, net::execution_context&>::value,
                        filesystem::path >::type & executable,
                Args && args,
                Inits && ... inits ) -> basic_process<typename ExecutionContext::executor_type>
    {
        return launcher_(context, ec,
                         executable,  
                         std::forward<Args>(args), 
                         std::get<Idx>(init_)...,
                         std::forward<Inits>(inits)...);
    }

    template<std::size_t ... Idx, typename Executor, typename Args, typename ... Inits>
    auto invoke(detail::index_sequence<Idx...>,
                Executor exec,
                const typename std::enable_if<
                        net::execution::is_executor<Executor>::value ||
                        net::is_executor<Executor>::value,
                        filesystem::path >::type & executable,
                Args && args,
                Inits && ... inits ) -> basic_process<Executor>
    {
        return launcher_(std::move(exec), 
                         executable,  
                         std::forward<Args>(args), 
                         std::get<Idx>(init_)...,
                         std::forward<Inits>(inits)...);
    }

    template<std::size_t ... Idx, typename Executor, typename Args, typename ... Inits>
    auto invoke(detail::index_sequence<Idx...>,
                Executor exec,
                error_code & ec,
                const typename std::enable_if<
                        net::execution::is_executor<Executor>::value ||
                        net::is_executor<Executor>::value,
                        filesystem::path >::type & executable,
                Args && args,
                Inits && ... inits ) -> basic_process<Executor>
    {
        return launcher_(std::move(exec), ec,
                         executable,  
                         std::forward<Args>(args), 
                         std::get<Idx>(init_)...,
                         std::forward<Inits>(inits)...);
    }

    Launcher launcher_;
    std::tuple<Init...> init_;
};


template<typename Launcher, typename ... Init>
auto bind_launcher(Launcher && launcher, Init && ... init)
    -> bound_launcher<typename std::decay<Launcher>::type,
                      typename std::decay<Init>::type...>
{
    return bound_launcher<typename std::decay<Launcher>::type,
                          typename std::decay<Init>::type...>(
                            std::forward<Launcher>(launcher), 
                            std::forward<Init>(init)...);
}

/// @brief  @overload bind_launcher(Launcher && launcher, Init && init)
/// @tparam ...Init The initializer types to bind to the default_launcher.
/// @param ...init The initializers types to bind to the default_launcher.
/// @return The new default_launcher.
template<typename ... Init>
auto bind_default_launcher(Init && ... init)
    -> bound_launcher<default_process_launcher,
                      typename std::decay<Init>::type...>
{
    return bound_launcher<default_process_launcher,
                          typename std::decay<Init>::type...>(
                            default_process_launcher(), 
                            std::forward<Init>(init)...);
}


BOOST_PROCESS_V2_END_NAMESPACE

#endif // BOOST_PROCESS_V2_BIND_LAUNCHER_HPP