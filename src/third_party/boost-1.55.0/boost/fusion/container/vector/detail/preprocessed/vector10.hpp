/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

    This is an auto-generated file. Do not edit!
==============================================================================*/
namespace boost { namespace fusion
{
    template <typename T0>
    struct vector_data1
    {
        vector_data1()
            : m0() {}
        vector_data1(
            typename detail::call_param<T0 >::type _0)
            : m0(_0) {}
        vector_data1(
            vector_data1 const& other)
            : m0(other.m0) {}
        vector_data1&
        operator=(vector_data1 const& vec)
        {
            this->m0 = vec.m0;
            return *this;
        }
        template <typename Sequence>
        static vector_data1
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            
            return vector_data1(*i0);
        }
        template <typename Sequence>
        static vector_data1
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            
            return vector_data1(*i0);
        }
        T0 m0;
    };
    template <typename T0>
    struct vector1
      : vector_data1<T0>
      , sequence_base<vector1<T0> >
    {
        typedef vector1<T0> this_type;
        typedef vector_data1<T0> base_type;
        typedef mpl::vector1<T0> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<1> size;
        vector1() {}
        explicit
        vector1(
            typename detail::call_param<T0 >::type _0)
            : base_type(_0) {}
        template <typename U0>
        vector1(
            vector1<U0> const& vec)
            : base_type(vec.m0) {}
        template <typename Sequence>
        vector1(
            Sequence const& seq
          , typename boost::disable_if<is_convertible<Sequence, T0> >::type* = 0
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector1(
            Sequence& seq
          , typename boost::disable_if<is_convertible<Sequence, T0> >::type* = 0
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0>
        vector1&
        operator=(vector1<U0> const& vec)
        {
            this->m0 = vec.m0;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            
            this->m0 = *i0;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1>
    struct vector_data2
    {
        vector_data2()
            : m0() , m1() {}
        vector_data2(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1)
            : m0(_0) , m1(_1) {}
        vector_data2(
            vector_data2 const& other)
            : m0(other.m0) , m1(other.m1) {}
        vector_data2&
        operator=(vector_data2 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1;
            return *this;
        }
        template <typename Sequence>
        static vector_data2
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0);
            return vector_data2(*i0 , *i1);
        }
        template <typename Sequence>
        static vector_data2
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0);
            return vector_data2(*i0 , *i1);
        }
        T0 m0; T1 m1;
    };
    template <typename T0 , typename T1>
    struct vector2
      : vector_data2<T0 , T1>
      , sequence_base<vector2<T0 , T1> >
    {
        typedef vector2<T0 , T1> this_type;
        typedef vector_data2<T0 , T1> base_type;
        typedef mpl::vector2<T0 , T1> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<2> size;
        vector2() {}
        vector2(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1)
            : base_type(_0 , _1) {}
        template <typename U0 , typename U1>
        vector2(
            vector2<U0 , U1> const& vec)
            : base_type(vec.m0 , vec.m1) {}
        template <typename Sequence>
        vector2(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector2(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1>
        vector2&
        operator=(vector2<U0 , U1> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0);
            this->m0 = *i0; this->m1 = *i1;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2>
    struct vector_data3
    {
        vector_data3()
            : m0() , m1() , m2() {}
        vector_data3(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2)
            : m0(_0) , m1(_1) , m2(_2) {}
        vector_data3(
            vector_data3 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) {}
        vector_data3&
        operator=(vector_data3 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2;
            return *this;
        }
        template <typename Sequence>
        static vector_data3
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1);
            return vector_data3(*i0 , *i1 , *i2);
        }
        template <typename Sequence>
        static vector_data3
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1);
            return vector_data3(*i0 , *i1 , *i2);
        }
        T0 m0; T1 m1; T2 m2;
    };
    template <typename T0 , typename T1 , typename T2>
    struct vector3
      : vector_data3<T0 , T1 , T2>
      , sequence_base<vector3<T0 , T1 , T2> >
    {
        typedef vector3<T0 , T1 , T2> this_type;
        typedef vector_data3<T0 , T1 , T2> base_type;
        typedef mpl::vector3<T0 , T1 , T2> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<3> size;
        vector3() {}
        vector3(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2)
            : base_type(_0 , _1 , _2) {}
        template <typename U0 , typename U1 , typename U2>
        vector3(
            vector3<U0 , U1 , U2> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2) {}
        template <typename Sequence>
        vector3(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector3(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2>
        vector3&
        operator=(vector3<U0 , U1 , U2> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2 , typename T3>
    struct vector_data4
    {
        vector_data4()
            : m0() , m1() , m2() , m3() {}
        vector_data4(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3)
            : m0(_0) , m1(_1) , m2(_2) , m3(_3) {}
        vector_data4(
            vector_data4 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) , m3(other.m3) {}
        vector_data4&
        operator=(vector_data4 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3;
            return *this;
        }
        template <typename Sequence>
        static vector_data4
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2);
            return vector_data4(*i0 , *i1 , *i2 , *i3);
        }
        template <typename Sequence>
        static vector_data4
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2);
            return vector_data4(*i0 , *i1 , *i2 , *i3);
        }
        T0 m0; T1 m1; T2 m2; T3 m3;
    };
    template <typename T0 , typename T1 , typename T2 , typename T3>
    struct vector4
      : vector_data4<T0 , T1 , T2 , T3>
      , sequence_base<vector4<T0 , T1 , T2 , T3> >
    {
        typedef vector4<T0 , T1 , T2 , T3> this_type;
        typedef vector_data4<T0 , T1 , T2 , T3> base_type;
        typedef mpl::vector4<T0 , T1 , T2 , T3> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<4> size;
        vector4() {}
        vector4(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3)
            : base_type(_0 , _1 , _2 , _3) {}
        template <typename U0 , typename U1 , typename U2 , typename U3>
        vector4(
            vector4<U0 , U1 , U2 , U3> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2 , vec.m3) {}
        template <typename Sequence>
        vector4(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector4(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2 , typename U3>
        vector4&
        operator=(vector4<U0 , U1 , U2 , U3> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2; this->m3 = *i3;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; } typename add_reference<T3>::type at_impl(mpl::int_<3>) { return this->m3; } typename add_reference<typename add_const<T3>::type>::type at_impl(mpl::int_<3>) const { return this->m3; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4>
    struct vector_data5
    {
        vector_data5()
            : m0() , m1() , m2() , m3() , m4() {}
        vector_data5(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4)
            : m0(_0) , m1(_1) , m2(_2) , m3(_3) , m4(_4) {}
        vector_data5(
            vector_data5 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) , m3(other.m3) , m4(other.m4) {}
        vector_data5&
        operator=(vector_data5 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4;
            return *this;
        }
        template <typename Sequence>
        static vector_data5
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3);
            return vector_data5(*i0 , *i1 , *i2 , *i3 , *i4);
        }
        template <typename Sequence>
        static vector_data5
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3);
            return vector_data5(*i0 , *i1 , *i2 , *i3 , *i4);
        }
        T0 m0; T1 m1; T2 m2; T3 m3; T4 m4;
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4>
    struct vector5
      : vector_data5<T0 , T1 , T2 , T3 , T4>
      , sequence_base<vector5<T0 , T1 , T2 , T3 , T4> >
    {
        typedef vector5<T0 , T1 , T2 , T3 , T4> this_type;
        typedef vector_data5<T0 , T1 , T2 , T3 , T4> base_type;
        typedef mpl::vector5<T0 , T1 , T2 , T3 , T4> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<5> size;
        vector5() {}
        vector5(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4)
            : base_type(_0 , _1 , _2 , _3 , _4) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4>
        vector5(
            vector5<U0 , U1 , U2 , U3 , U4> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2 , vec.m3 , vec.m4) {}
        template <typename Sequence>
        vector5(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector5(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4>
        vector5&
        operator=(vector5<U0 , U1 , U2 , U3 , U4> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2; this->m3 = *i3; this->m4 = *i4;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; } typename add_reference<T3>::type at_impl(mpl::int_<3>) { return this->m3; } typename add_reference<typename add_const<T3>::type>::type at_impl(mpl::int_<3>) const { return this->m3; } typename add_reference<T4>::type at_impl(mpl::int_<4>) { return this->m4; } typename add_reference<typename add_const<T4>::type>::type at_impl(mpl::int_<4>) const { return this->m4; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5>
    struct vector_data6
    {
        vector_data6()
            : m0() , m1() , m2() , m3() , m4() , m5() {}
        vector_data6(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5)
            : m0(_0) , m1(_1) , m2(_2) , m3(_3) , m4(_4) , m5(_5) {}
        vector_data6(
            vector_data6 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) , m3(other.m3) , m4(other.m4) , m5(other.m5) {}
        vector_data6&
        operator=(vector_data6 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5;
            return *this;
        }
        template <typename Sequence>
        static vector_data6
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4);
            return vector_data6(*i0 , *i1 , *i2 , *i3 , *i4 , *i5);
        }
        template <typename Sequence>
        static vector_data6
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4);
            return vector_data6(*i0 , *i1 , *i2 , *i3 , *i4 , *i5);
        }
        T0 m0; T1 m1; T2 m2; T3 m3; T4 m4; T5 m5;
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5>
    struct vector6
      : vector_data6<T0 , T1 , T2 , T3 , T4 , T5>
      , sequence_base<vector6<T0 , T1 , T2 , T3 , T4 , T5> >
    {
        typedef vector6<T0 , T1 , T2 , T3 , T4 , T5> this_type;
        typedef vector_data6<T0 , T1 , T2 , T3 , T4 , T5> base_type;
        typedef mpl::vector6<T0 , T1 , T2 , T3 , T4 , T5> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<6> size;
        vector6() {}
        vector6(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5)
            : base_type(_0 , _1 , _2 , _3 , _4 , _5) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5>
        vector6(
            vector6<U0 , U1 , U2 , U3 , U4 , U5> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2 , vec.m3 , vec.m4 , vec.m5) {}
        template <typename Sequence>
        vector6(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector6(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5>
        vector6&
        operator=(vector6<U0 , U1 , U2 , U3 , U4 , U5> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2; this->m3 = *i3; this->m4 = *i4; this->m5 = *i5;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; } typename add_reference<T3>::type at_impl(mpl::int_<3>) { return this->m3; } typename add_reference<typename add_const<T3>::type>::type at_impl(mpl::int_<3>) const { return this->m3; } typename add_reference<T4>::type at_impl(mpl::int_<4>) { return this->m4; } typename add_reference<typename add_const<T4>::type>::type at_impl(mpl::int_<4>) const { return this->m4; } typename add_reference<T5>::type at_impl(mpl::int_<5>) { return this->m5; } typename add_reference<typename add_const<T5>::type>::type at_impl(mpl::int_<5>) const { return this->m5; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6>
    struct vector_data7
    {
        vector_data7()
            : m0() , m1() , m2() , m3() , m4() , m5() , m6() {}
        vector_data7(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6)
            : m0(_0) , m1(_1) , m2(_2) , m3(_3) , m4(_4) , m5(_5) , m6(_6) {}
        vector_data7(
            vector_data7 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) , m3(other.m3) , m4(other.m4) , m5(other.m5) , m6(other.m6) {}
        vector_data7&
        operator=(vector_data7 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6;
            return *this;
        }
        template <typename Sequence>
        static vector_data7
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5);
            return vector_data7(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6);
        }
        template <typename Sequence>
        static vector_data7
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5);
            return vector_data7(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6);
        }
        T0 m0; T1 m1; T2 m2; T3 m3; T4 m4; T5 m5; T6 m6;
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6>
    struct vector7
      : vector_data7<T0 , T1 , T2 , T3 , T4 , T5 , T6>
      , sequence_base<vector7<T0 , T1 , T2 , T3 , T4 , T5 , T6> >
    {
        typedef vector7<T0 , T1 , T2 , T3 , T4 , T5 , T6> this_type;
        typedef vector_data7<T0 , T1 , T2 , T3 , T4 , T5 , T6> base_type;
        typedef mpl::vector7<T0 , T1 , T2 , T3 , T4 , T5 , T6> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<7> size;
        vector7() {}
        vector7(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6)
            : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6>
        vector7(
            vector7<U0 , U1 , U2 , U3 , U4 , U5 , U6> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2 , vec.m3 , vec.m4 , vec.m5 , vec.m6) {}
        template <typename Sequence>
        vector7(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector7(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6>
        vector7&
        operator=(vector7<U0 , U1 , U2 , U3 , U4 , U5 , U6> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2; this->m3 = *i3; this->m4 = *i4; this->m5 = *i5; this->m6 = *i6;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; } typename add_reference<T3>::type at_impl(mpl::int_<3>) { return this->m3; } typename add_reference<typename add_const<T3>::type>::type at_impl(mpl::int_<3>) const { return this->m3; } typename add_reference<T4>::type at_impl(mpl::int_<4>) { return this->m4; } typename add_reference<typename add_const<T4>::type>::type at_impl(mpl::int_<4>) const { return this->m4; } typename add_reference<T5>::type at_impl(mpl::int_<5>) { return this->m5; } typename add_reference<typename add_const<T5>::type>::type at_impl(mpl::int_<5>) const { return this->m5; } typename add_reference<T6>::type at_impl(mpl::int_<6>) { return this->m6; } typename add_reference<typename add_const<T6>::type>::type at_impl(mpl::int_<6>) const { return this->m6; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6 , typename T7>
    struct vector_data8
    {
        vector_data8()
            : m0() , m1() , m2() , m3() , m4() , m5() , m6() , m7() {}
        vector_data8(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7)
            : m0(_0) , m1(_1) , m2(_2) , m3(_3) , m4(_4) , m5(_5) , m6(_6) , m7(_7) {}
        vector_data8(
            vector_data8 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) , m3(other.m3) , m4(other.m4) , m5(other.m5) , m6(other.m6) , m7(other.m7) {}
        vector_data8&
        operator=(vector_data8 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6; this->m7 = vec.m7;
            return *this;
        }
        template <typename Sequence>
        static vector_data8
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6);
            return vector_data8(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6 , *i7);
        }
        template <typename Sequence>
        static vector_data8
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6);
            return vector_data8(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6 , *i7);
        }
        T0 m0; T1 m1; T2 m2; T3 m3; T4 m4; T5 m5; T6 m6; T7 m7;
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6 , typename T7>
    struct vector8
      : vector_data8<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7>
      , sequence_base<vector8<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7> >
    {
        typedef vector8<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7> this_type;
        typedef vector_data8<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7> base_type;
        typedef mpl::vector8<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<8> size;
        vector8() {}
        vector8(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7)
            : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6 , _7) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7>
        vector8(
            vector8<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2 , vec.m3 , vec.m4 , vec.m5 , vec.m6 , vec.m7) {}
        template <typename Sequence>
        vector8(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector8(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7>
        vector8&
        operator=(vector8<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6; this->m7 = vec.m7;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2; this->m3 = *i3; this->m4 = *i4; this->m5 = *i5; this->m6 = *i6; this->m7 = *i7;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; } typename add_reference<T3>::type at_impl(mpl::int_<3>) { return this->m3; } typename add_reference<typename add_const<T3>::type>::type at_impl(mpl::int_<3>) const { return this->m3; } typename add_reference<T4>::type at_impl(mpl::int_<4>) { return this->m4; } typename add_reference<typename add_const<T4>::type>::type at_impl(mpl::int_<4>) const { return this->m4; } typename add_reference<T5>::type at_impl(mpl::int_<5>) { return this->m5; } typename add_reference<typename add_const<T5>::type>::type at_impl(mpl::int_<5>) const { return this->m5; } typename add_reference<T6>::type at_impl(mpl::int_<6>) { return this->m6; } typename add_reference<typename add_const<T6>::type>::type at_impl(mpl::int_<6>) const { return this->m6; } typename add_reference<T7>::type at_impl(mpl::int_<7>) { return this->m7; } typename add_reference<typename add_const<T7>::type>::type at_impl(mpl::int_<7>) const { return this->m7; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6 , typename T7 , typename T8>
    struct vector_data9
    {
        vector_data9()
            : m0() , m1() , m2() , m3() , m4() , m5() , m6() , m7() , m8() {}
        vector_data9(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7 , typename detail::call_param<T8 >::type _8)
            : m0(_0) , m1(_1) , m2(_2) , m3(_3) , m4(_4) , m5(_5) , m6(_6) , m7(_7) , m8(_8) {}
        vector_data9(
            vector_data9 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) , m3(other.m3) , m4(other.m4) , m5(other.m5) , m6(other.m6) , m7(other.m7) , m8(other.m8) {}
        vector_data9&
        operator=(vector_data9 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6; this->m7 = vec.m7; this->m8 = vec.m8;
            return *this;
        }
        template <typename Sequence>
        static vector_data9
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6); typedef typename result_of::next< I7>::type I8; I8 i8 = fusion::next(i7);
            return vector_data9(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6 , *i7 , *i8);
        }
        template <typename Sequence>
        static vector_data9
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6); typedef typename result_of::next< I7>::type I8; I8 i8 = fusion::next(i7);
            return vector_data9(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6 , *i7 , *i8);
        }
        T0 m0; T1 m1; T2 m2; T3 m3; T4 m4; T5 m5; T6 m6; T7 m7; T8 m8;
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6 , typename T7 , typename T8>
    struct vector9
      : vector_data9<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8>
      , sequence_base<vector9<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8> >
    {
        typedef vector9<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8> this_type;
        typedef vector_data9<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8> base_type;
        typedef mpl::vector9<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<9> size;
        vector9() {}
        vector9(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7 , typename detail::call_param<T8 >::type _8)
            : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6 , _7 , _8) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8>
        vector9(
            vector9<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2 , vec.m3 , vec.m4 , vec.m5 , vec.m6 , vec.m7 , vec.m8) {}
        template <typename Sequence>
        vector9(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector9(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8>
        vector9&
        operator=(vector9<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6; this->m7 = vec.m7; this->m8 = vec.m8;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6); typedef typename result_of::next< I7>::type I8; I8 i8 = fusion::next(i7);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2; this->m3 = *i3; this->m4 = *i4; this->m5 = *i5; this->m6 = *i6; this->m7 = *i7; this->m8 = *i8;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; } typename add_reference<T3>::type at_impl(mpl::int_<3>) { return this->m3; } typename add_reference<typename add_const<T3>::type>::type at_impl(mpl::int_<3>) const { return this->m3; } typename add_reference<T4>::type at_impl(mpl::int_<4>) { return this->m4; } typename add_reference<typename add_const<T4>::type>::type at_impl(mpl::int_<4>) const { return this->m4; } typename add_reference<T5>::type at_impl(mpl::int_<5>) { return this->m5; } typename add_reference<typename add_const<T5>::type>::type at_impl(mpl::int_<5>) const { return this->m5; } typename add_reference<T6>::type at_impl(mpl::int_<6>) { return this->m6; } typename add_reference<typename add_const<T6>::type>::type at_impl(mpl::int_<6>) const { return this->m6; } typename add_reference<T7>::type at_impl(mpl::int_<7>) { return this->m7; } typename add_reference<typename add_const<T7>::type>::type at_impl(mpl::int_<7>) const { return this->m7; } typename add_reference<T8>::type at_impl(mpl::int_<8>) { return this->m8; } typename add_reference<typename add_const<T8>::type>::type at_impl(mpl::int_<8>) const { return this->m8; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6 , typename T7 , typename T8 , typename T9>
    struct vector_data10
    {
        vector_data10()
            : m0() , m1() , m2() , m3() , m4() , m5() , m6() , m7() , m8() , m9() {}
        vector_data10(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7 , typename detail::call_param<T8 >::type _8 , typename detail::call_param<T9 >::type _9)
            : m0(_0) , m1(_1) , m2(_2) , m3(_3) , m4(_4) , m5(_5) , m6(_6) , m7(_7) , m8(_8) , m9(_9) {}
        vector_data10(
            vector_data10 const& other)
            : m0(other.m0) , m1(other.m1) , m2(other.m2) , m3(other.m3) , m4(other.m4) , m5(other.m5) , m6(other.m6) , m7(other.m7) , m8(other.m8) , m9(other.m9) {}
        vector_data10&
        operator=(vector_data10 const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6; this->m7 = vec.m7; this->m8 = vec.m8; this->m9 = vec.m9;
            return *this;
        }
        template <typename Sequence>
        static vector_data10
        init_from_sequence(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6); typedef typename result_of::next< I7>::type I8; I8 i8 = fusion::next(i7); typedef typename result_of::next< I8>::type I9; I9 i9 = fusion::next(i8);
            return vector_data10(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6 , *i7 , *i8 , *i9);
        }
        template <typename Sequence>
        static vector_data10
        init_from_sequence(Sequence& seq)
        {
            typedef typename result_of::begin<Sequence>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6); typedef typename result_of::next< I7>::type I8; I8 i8 = fusion::next(i7); typedef typename result_of::next< I8>::type I9; I9 i9 = fusion::next(i8);
            return vector_data10(*i0 , *i1 , *i2 , *i3 , *i4 , *i5 , *i6 , *i7 , *i8 , *i9);
        }
        T0 m0; T1 m1; T2 m2; T3 m3; T4 m4; T5 m5; T6 m6; T7 m7; T8 m8; T9 m9;
    };
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6 , typename T7 , typename T8 , typename T9>
    struct vector10
      : vector_data10<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8 , T9>
      , sequence_base<vector10<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8 , T9> >
    {
        typedef vector10<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8 , T9> this_type;
        typedef vector_data10<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8 , T9> base_type;
        typedef mpl::vector10<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8 , T9> types;
        typedef vector_tag fusion_tag;
        typedef fusion_sequence_tag tag; 
        typedef mpl::false_ is_view;
        typedef random_access_traversal_tag category;
        typedef mpl::int_<10> size;
        vector10() {}
        vector10(
            typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7 , typename detail::call_param<T8 >::type _8 , typename detail::call_param<T9 >::type _9)
            : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6 , _7 , _8 , _9) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8 , typename U9>
        vector10(
            vector10<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8 , U9> const& vec)
            : base_type(vec.m0 , vec.m1 , vec.m2 , vec.m3 , vec.m4 , vec.m5 , vec.m6 , vec.m7 , vec.m8 , vec.m9) {}
        template <typename Sequence>
        vector10(
            Sequence const& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename Sequence>
        vector10(
            Sequence& seq
            )
            : base_type(base_type::init_from_sequence(seq)) {}
        template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8 , typename U9>
        vector10&
        operator=(vector10<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8 , U9> const& vec)
        {
            this->m0 = vec.m0; this->m1 = vec.m1; this->m2 = vec.m2; this->m3 = vec.m3; this->m4 = vec.m4; this->m5 = vec.m5; this->m6 = vec.m6; this->m7 = vec.m7; this->m8 = vec.m8; this->m9 = vec.m9;
            return *this;
        }
        template <typename Sequence>
        typename boost::disable_if<is_convertible<Sequence, T0>, this_type&>::type
        operator=(Sequence const& seq)
        {
            typedef typename result_of::begin<Sequence const>::type I0;
            I0 i0 = fusion::begin(seq);
            typedef typename result_of::next< I0>::type I1; I1 i1 = fusion::next(i0); typedef typename result_of::next< I1>::type I2; I2 i2 = fusion::next(i1); typedef typename result_of::next< I2>::type I3; I3 i3 = fusion::next(i2); typedef typename result_of::next< I3>::type I4; I4 i4 = fusion::next(i3); typedef typename result_of::next< I4>::type I5; I5 i5 = fusion::next(i4); typedef typename result_of::next< I5>::type I6; I6 i6 = fusion::next(i5); typedef typename result_of::next< I6>::type I7; I7 i7 = fusion::next(i6); typedef typename result_of::next< I7>::type I8; I8 i8 = fusion::next(i7); typedef typename result_of::next< I8>::type I9; I9 i9 = fusion::next(i8);
            this->m0 = *i0; this->m1 = *i1; this->m2 = *i2; this->m3 = *i3; this->m4 = *i4; this->m5 = *i5; this->m6 = *i6; this->m7 = *i7; this->m8 = *i8; this->m9 = *i9;
            return *this;
        }
        typename add_reference<T0>::type at_impl(mpl::int_<0>) { return this->m0; } typename add_reference<typename add_const<T0>::type>::type at_impl(mpl::int_<0>) const { return this->m0; } typename add_reference<T1>::type at_impl(mpl::int_<1>) { return this->m1; } typename add_reference<typename add_const<T1>::type>::type at_impl(mpl::int_<1>) const { return this->m1; } typename add_reference<T2>::type at_impl(mpl::int_<2>) { return this->m2; } typename add_reference<typename add_const<T2>::type>::type at_impl(mpl::int_<2>) const { return this->m2; } typename add_reference<T3>::type at_impl(mpl::int_<3>) { return this->m3; } typename add_reference<typename add_const<T3>::type>::type at_impl(mpl::int_<3>) const { return this->m3; } typename add_reference<T4>::type at_impl(mpl::int_<4>) { return this->m4; } typename add_reference<typename add_const<T4>::type>::type at_impl(mpl::int_<4>) const { return this->m4; } typename add_reference<T5>::type at_impl(mpl::int_<5>) { return this->m5; } typename add_reference<typename add_const<T5>::type>::type at_impl(mpl::int_<5>) const { return this->m5; } typename add_reference<T6>::type at_impl(mpl::int_<6>) { return this->m6; } typename add_reference<typename add_const<T6>::type>::type at_impl(mpl::int_<6>) const { return this->m6; } typename add_reference<T7>::type at_impl(mpl::int_<7>) { return this->m7; } typename add_reference<typename add_const<T7>::type>::type at_impl(mpl::int_<7>) const { return this->m7; } typename add_reference<T8>::type at_impl(mpl::int_<8>) { return this->m8; } typename add_reference<typename add_const<T8>::type>::type at_impl(mpl::int_<8>) const { return this->m8; } typename add_reference<T9>::type at_impl(mpl::int_<9>) { return this->m9; } typename add_reference<typename add_const<T9>::type>::type at_impl(mpl::int_<9>) const { return this->m9; }
        template<typename I>
        typename add_reference<typename mpl::at<types, I>::type>::type
        at_impl(I)
        {
            return this->at_impl(mpl::int_<I::value>());
        }
        template<typename I>
        typename add_reference<typename add_const<typename mpl::at<types, I>::type>::type>::type
        at_impl(I) const
        {
            return this->at_impl(mpl::int_<I::value>());
        }
    };
}}
