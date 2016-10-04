/*=============================================================================
    Copyright (c) 2014-2015 Kohei Takahashi

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#ifndef FUSION_VECTOR_11052014_1625
#define FUSION_VECTOR_11052014_1625

#include <boost/config.hpp>
#include <boost/fusion/support/config.hpp>
#include <boost/fusion/container/vector/detail/config.hpp>
#include <boost/fusion/container/vector/vector_fwd.hpp>

///////////////////////////////////////////////////////////////////////////////
// Without variadics, we will use the PP version
///////////////////////////////////////////////////////////////////////////////
#if !defined(BOOST_FUSION_HAS_VARIADIC_VECTOR)
# include <boost/fusion/container/vector/detail/cpp03/vector.hpp>
#else

///////////////////////////////////////////////////////////////////////////////
// C++11 interface
///////////////////////////////////////////////////////////////////////////////
#include <boost/fusion/support/sequence_base.hpp>
#include <boost/fusion/support/is_sequence.hpp>
#include <boost/fusion/support/detail/index_sequence.hpp>
#include <boost/fusion/container/vector/detail/at_impl.hpp>
#include <boost/fusion/container/vector/detail/value_at_impl.hpp>
#include <boost/fusion/container/vector/detail/begin_impl.hpp>
#include <boost/fusion/container/vector/detail/end_impl.hpp>
#include <boost/fusion/sequence/intrinsic/size.hpp>
#include <boost/fusion/sequence/intrinsic/begin.hpp>
#include <boost/fusion/iterator/advance.hpp>
#include <boost/fusion/iterator/deref.hpp>
#include <boost/core/enable_if.hpp>
#include <boost/mpl/int.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/remove_cv.hpp>
#include <boost/type_traits/remove_reference.hpp>
#include <cstddef>
#include <utility>

namespace boost { namespace fusion
{
    struct vector_tag;
    struct random_access_traversal_tag;

    namespace vector_detail
    {
        struct each_elem {};
        struct copy_or_move {};
        template <typename I> struct from_sequence {};

        template <typename Sequence>
        struct make_indices_from_seq
            : detail::make_index_sequence<
                  fusion::result_of::size<typename remove_reference<Sequence>::type>::value
              >
        {};

        template <typename T>
        struct pure : remove_cv<typename remove_reference<T>::type> {};

        template <typename Sequence, typename This, int = result_of::size<This>::value>
        struct is_convertible_to_first
            : boost::is_convertible<Sequence, typename result_of::value_at_c<This, 0>::type>
        {};

        template <typename Sequence, typename This>
        struct is_convertible_to_first<Sequence, This, 0>
            : mpl::false_
        {};

        template <typename This, typename ...T>
        BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
        inline each_elem
        dispatch(T const&...) BOOST_NOEXCEPT { return each_elem(); }

        template <typename This>
        BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
        inline copy_or_move
        dispatch(This const&) BOOST_NOEXCEPT { return copy_or_move(); }

        template <typename This, typename Sequence>
        BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
        inline from_sequence<
            typename lazy_enable_if_c<
                (traits::is_sequence<typename remove_reference<Sequence>::type>::value &&
                 !is_same<This, typename pure<Sequence>::type>::value &&
                 !is_convertible_to_first<Sequence, This>::value)
              , make_indices_from_seq<Sequence>
            >::type
        >
        dispatch(Sequence&&) BOOST_NOEXCEPT
        { return from_sequence<typename make_indices_from_seq<Sequence>::type>(); }


        // forward_at_c allows to access Nth element even if ForwardSequence
        // since fusion::at_c requires RandomAccessSequence.
        namespace result_of
        {
            template <typename Sequence, int N>
            struct forward_at_c
                : fusion::result_of::deref<
                      typename fusion::result_of::advance_c<
                          typename fusion::result_of::begin<
                              typename remove_reference<Sequence>::type
                          >::type
                        , N
                      >::type
                  >
            {};
        }

        template <int N, typename Sequence>
        BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
        inline typename result_of::forward_at_c<Sequence, N>::type
        forward_at_c(Sequence&& seq)
        {
            typedef typename
                result_of::forward_at_c<Sequence, N>::type
            result;
            return std::forward<result>(*advance_c<N>(begin(seq)));
        }

        // Object proxy since preserve object order
        template <std::size_t, typename T>
        struct store
        {
            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            store()
                : elem() // value-initialized explicitly
            {}

            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            store(store const& rhs)
                : elem(rhs.get())
            {}

            BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            store&
            operator=(store const& rhs)
            {
                elem = rhs.get();
                return *this;
            }

            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            store(store&& rhs)
                : elem(static_cast<T&&>(rhs.get()))
            {}

            BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            store&
            operator=(store&& rhs)
            {
                elem = static_cast<T&&>(rhs.get());
                return *this;
            }

            template <typename U>
            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            store(U&& rhs
                , typename disable_if<is_same<typename pure<U>::type, store> >::type* = 0)
                : elem(std::forward<U>(rhs))
            {}

            template <typename U>
            BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            typename disable_if<is_same<typename pure<U>::type, store>, store&>::type
            operator=(U&& rhs)
            {
                elem = std::forward<U>(rhs);
                return *this;
            }

            BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            T      & get()       { return elem; }
            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            T const& get() const { return elem; }

            T elem;
        };

        template <typename I, typename ...T>
        struct vector_data;

        template <std::size_t ...I, typename ...T>
        struct vector_data<detail::index_sequence<I...>, T...>
            : store<I, T>...
            , sequence_base<vector_data<detail::index_sequence<I...>, T...> >
        {
            typedef vector_tag                  fusion_tag;
            typedef fusion_sequence_tag         tag; // this gets picked up by MPL
            typedef mpl::false_                 is_view;
            typedef random_access_traversal_tag category;
            typedef mpl::int_<sizeof...(T)>     size;
            typedef vector<T...>                type_sequence;

            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            vector_data()
            {}

            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            vector_data(copy_or_move, vector_data const& rhs)
                : store<I, T>(static_cast<store<I, T> const&>(rhs))...
            {}

            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            vector_data(copy_or_move, vector_data&& rhs)
                : store<I, T>(std::forward<store<I, T> >(rhs))...
            {}

            template <typename Sequence>
            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            explicit
            vector_data(from_sequence<detail::index_sequence<I...> >, Sequence&& rhs)
                : store<I, T>(forward_at_c<I>(rhs))...
            {}

            template <typename ...U>
            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            explicit
            vector_data(each_elem, U&&... var)
                : store<I, T>(std::forward<U>(var))...
            {}

            template <typename Sequence>
            BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            void
            assign(Sequence&&, detail::index_sequence<>) {}

            template <typename Sequence, std::size_t N, std::size_t ...M>
            BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            void
            assign(Sequence&& seq, detail::index_sequence<N, M...>)
            {
                at_impl(mpl::int_<N>()) = vector_detail::forward_at_c<N>(seq);
                assign(std::forward<Sequence>(seq), detail::index_sequence<M...>());
            }

            template <std::size_t N, typename U>
            static BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            auto at_detail(store<N, U>* this_) -> decltype(this_->get())
            {
                return this_->get();
            }

            template <std::size_t N, typename U>
            static BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            auto at_detail(store<N, U> const* this_) -> decltype(this_->get())
            {
                return this_->get();
            }

            template <typename J>
            BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            auto at_impl(J) -> decltype(at_detail<J::value>(this))
            {
                return at_detail<J::value>(this);
            }

            template <typename J>
            BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
            auto at_impl(J) const -> decltype(at_detail<J::value>(this))
            {
                return at_detail<J::value>(this);
            }

            template <std::size_t N, typename U>
            static BOOST_FUSION_GPU_ENABLED
            mpl::identity<U> value_at_impl(store<N, U>*);
        };

        template <typename V, typename... T>
        struct trim_void_;

        template <typename... T>
        struct trim_void_<vector<T...> >
        {
            typedef vector_data<
                typename detail::make_index_sequence<sizeof...(T)>::type
              , T...
            > type;
        };

        template <typename... T, typename... Tail>
        struct trim_void_<vector<T...>, void_, Tail...>
            : trim_void_<vector<T...> > {};

        template <typename... T, typename Head, typename... Tail>
        struct trim_void_<vector<T...>, Head, Tail...>
            : trim_void_<vector<T..., Head>, Tail...> {};

        template <typename... T>
        struct trim_void : trim_void_<vector<>, T...> {};
    } // namespace boost::fusion::vector_detail

    // This class provides backward compatibility: vector<T, ..., void_, void_, ...>.
    template <typename... T>
    struct vector
        : vector_detail::trim_void<T...>::type
    {
        typedef typename vector_detail::trim_void<T...>::type base;

        BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
        vector()
        {}

        // rvalue-references is required here in order to forward any arguments to
        // base: vector(T const&...) doesn't work with trailing void_ and
        // vector(U const&...) cannot forward any arguments to base.
        template <typename... U>
        // XXX: constexpr become error due to pull-request #79, booooo!!
        //      In the (near) future release, should be fixed.
        /* BOOST_CONSTEXPR */ BOOST_FUSION_GPU_ENABLED
        vector(U&&... u)
            : base(vector_detail::dispatch<vector>(std::forward<U>(u)...), std::forward<U>(u)...)
        {}

        template <typename Sequence>
        BOOST_CXX14_CONSTEXPR BOOST_FUSION_GPU_ENABLED
        vector&
        operator=(Sequence&& rhs)
        {
            typedef typename
                vector_detail::make_indices_from_seq<Sequence>::type
            indices;
            base::assign(std::forward<Sequence>(rhs), indices());
            return *this;
        }
    };
}}

#endif
#endif

