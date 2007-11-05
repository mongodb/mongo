#ifndef BOOST_ARCHIVE_DETAIL_ISERIALIZER_HPP
#define BOOST_ARCHIVE_DETAIL_ISERIALIZER_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#pragma inline_depth(511)
#pragma inline_recursion(on)
#endif

#if defined(__MWERKS__)
#pragma inline_depth(511)
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// iserializer.hpp: interface for serialization system.

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <new>     // for placement new
#include <memory>  // for auto_ptr
#include <cstddef> // size_t

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ 
    using ::size_t; 
} // namespace std
#endif
#include <boost/throw_exception.hpp>
#include <boost/smart_cast.hpp>
#include <boost/static_assert.hpp>
#include <boost/static_warning.hpp>
#include <boost/detail/no_exceptions_support.hpp>

#include <boost/type_traits/is_pointer.hpp>
#include <boost/type_traits/is_fundamental.hpp>
#include <boost/type_traits/is_enum.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/remove_const.hpp>
#include <boost/serialization/is_abstract.hpp>

#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/or.hpp>
#include <boost/mpl/and.hpp>
#include <boost/mpl/less.hpp>
#include <boost/mpl/greater_equal.hpp>
#include <boost/mpl/int.hpp>
#include <boost/mpl/list.hpp>
#include <boost/mpl/empty.hpp>
#include <boost/mpl/not.hpp>

 #ifndef BOOST_SERIALIZATION_DEFAULT_TYPE_INFO   
     #include <boost/serialization/extended_type_info_typeid.hpp>   
 #endif 
// the following is need only for dynamic cast of polymorphic pointers
#include <boost/archive/detail/basic_iarchive.hpp>
#include <boost/archive/detail/basic_iserializer.hpp>
#include <boost/archive/detail/archive_pointer_iserializer.hpp>
#include <boost/archive/archive_exception.hpp>

#include <boost/serialization/force_include.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/level.hpp>
#include <boost/serialization/tracking.hpp>
#include <boost/serialization/type_info_implementation.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/serialization/void_cast.hpp>

namespace boost {

namespace serialization {
    class extended_type_info;
} // namespace serialization

namespace archive {

// an accessor to permit friend access to archives.  Needed because
// some compilers don't handle friend templates completely
class load_access {
public:
    template<class Archive, class T>
    static void load_primitive(Archive &ar, T &t){
        ar.load(t);
    }
};

namespace detail {

template<class Archive, class T>
class iserializer : public basic_iserializer
{
private:
    virtual void destroy(/*const*/ void *address) const {
        boost::serialization::access::destroy(static_cast<T *>(address));
    }
    // private constructor to inhibit any existence other than the 
    // static one
    explicit iserializer() :
        basic_iserializer(
            * boost::serialization::type_info_implementation<T>::type::get_instance()
        )
    {}
public:
    virtual BOOST_DLLEXPORT void load_object_data(
        basic_iarchive & ar,
        void *x, 
        const unsigned int file_version
    ) const BOOST_USED ;
    virtual bool class_info() const {
        return boost::serialization::implementation_level<T>::value 
            >= boost::serialization::object_class_info;
    }
    virtual bool tracking(const unsigned int /* flags */) const {
//        if(0 != (flags & no_tracking))
//            return false;
        return boost::serialization::tracking_level<T>::value 
                == boost::serialization::track_always
            || boost::serialization::tracking_level<T>::value 
                == boost::serialization::track_selectivly
            && serialized_as_pointer();
    }
    virtual unsigned int version() const {
        return ::boost::serialization::version<T>::value;
    }
    virtual bool is_polymorphic() const {
        typedef BOOST_DEDUCED_TYPENAME 
            boost::serialization::type_info_implementation<
                T
            >::type::is_polymorphic::type typex;
        return typex::value;
    }
    static iserializer & instantiate(){
        static iserializer instance;
        return instance;
    }
    virtual ~iserializer(){};
};

template<class Archive, class T>
BOOST_DLLEXPORT void iserializer<Archive, T>::load_object_data(
    basic_iarchive & ar,
    void *x, 
    const unsigned int file_version
) const {
    // make sure call is routed through the higest interface that might
    // be specialized by the user.
    boost::serialization::serialize_adl(
        boost::smart_cast_reference<Archive &>(ar),
        * static_cast<T *>(x), 
        file_version
    );
}

// instantiation of this template creates a static object.  Note inversion of
// normal argument order to workaround bizarre error in MSVC 6.0 which only
// manifests iftself during compiler time.
template<class T, class Archive>
class pointer_iserializer : public archive_pointer_iserializer<Archive> 
{
private:
    virtual const basic_iserializer & get_basic_serializer() const {
        return iserializer<Archive, T>::instantiate();
    }
    virtual BOOST_DLLEXPORT void load_object_ptr(
        basic_iarchive & ar, 
        void * & x,
        const unsigned int file_version
    ) const BOOST_USED;
#if defined(__GNUC__) || ( defined(BOOST_MSVC) && (_MSC_VER <= 1300) )
public:
#endif
    // private constructor to inhibit any existence other than the 
    // static one.  Note GCC doesn't permit constructor to be private
    explicit BOOST_DLLEXPORT pointer_iserializer() BOOST_USED;
    static const pointer_iserializer instance;
public:
    // at least one compiler (CW) seems to require that serialize_adl
    // be explicitly instantiated. Still under investigation. 
    #if ! BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582))
    void (* const m)(Archive &, T &, const unsigned);
    boost::serialization::extended_type_info * (* e)();
    #endif
    static BOOST_DLLEXPORT const pointer_iserializer & instantiate() BOOST_USED;
    virtual ~pointer_iserializer(){};
};

template<class T, class Archive>
BOOST_DLLEXPORT const pointer_iserializer<T, Archive> & 
pointer_iserializer<T, Archive>::instantiate() {
    return instance;
}

// note: instances of this template to be constructed before the main
// is called in order for things to be initialized properly.  For this
// reason, hiding the instance in a static function as was done above
// won't work here so we created a free instance here.
template<class T, class Archive>
const pointer_iserializer<T, Archive> pointer_iserializer<T, Archive>::instance;

// note trick to be sure that operator new is using class specific
// version if such exists. Due to Peter Dimov.
// note: the following fails if T has no default constructor.
// otherwise it would have been ideal
//struct heap_allocator : public T 
//{
//    T * invoke(){
//        return ::new(sizeof(T));
//    }
//}

// note: this should really be a member of the load_ptr function
// below but some compilers still complain about this.
template<class T>
struct heap_allocator
{
    #if 0
        // note: this fails on msvc 7.0 and gcc 3.2
        template <class U, U x> struct test;
        typedef char* yes;
        typedef int* no;
        template <class U>
        yes has_op_new(U*, test<void* (*)(std::size_t), &U::operator new>* = 0);
        no has_op_new(...);

        template<class U>
        T * new_operator(U);

        T * new_operator(yes){
            return (T::operator new)(sizeof(T));
        }
        T * new_operator(no){
            return static_cast<T *>(operator new(sizeof(T)));
        }
        static T * invoke(){
            return new_operator(has_op_new(static_cast<T *>(NULL)));
        }
    #else
        // while this doesn't handle operator new overload for class T
        static T * invoke(){
            return static_cast<T *>(operator new(sizeof(T)));
        }
    #endif
};

// due to Martin Ecker
template <typename T>
class auto_ptr_with_deleter
{
public:
    explicit auto_ptr_with_deleter(T* p) :
        m_p(p)
    {}
    ~auto_ptr_with_deleter(){
        if (m_p)
            boost::serialization::access::destroy(m_p);
    }
    T* get() const {
        return m_p;
    }

    T* release() {
        T* p = m_p;
        m_p = NULL;
        return p;
    }
private:
    T* m_p;
};

template<class T, class Archive>
BOOST_DLLEXPORT void pointer_iserializer<T, Archive>::load_object_ptr(
    basic_iarchive & ar, 
    void * & x,
    const unsigned int file_version
) const {
    Archive & ar_impl = boost::smart_cast_reference<Archive &>(ar);

//    if(0 != (ar.get_flags() & no_object_creation)){
//        ar_impl >> boost::serialization::make_nvp(NULL, * static_cast<T *>(x));
//        return;
//    }

    auto_ptr_with_deleter<T> ap(heap_allocator<T>::invoke());
    if(NULL == ap.get())
        boost::throw_exception(std::bad_alloc()) ;

    T * t = ap.get();
    x = t;

    // catch exception during load_construct_data so that we don't
    // automatically delete the t which is most likely not fully
    // constructed
    BOOST_TRY {
        // this addresses an obscure situtation that occurs when 
        // load_constructor de-serializes something through a pointer.
        ar.next_object_pointer(t);
        boost::serialization::load_construct_data_adl<Archive, T>(
            ar_impl,
            t, 
            file_version
        );
    }
    BOOST_CATCH(...){
        BOOST_RETHROW;
    }
    BOOST_CATCH_END

    ar_impl >> boost::serialization::make_nvp(NULL, * t);
    ap.release();
}

template<class T, class Archive>
#if ! BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582))
BOOST_DLLEXPORT pointer_iserializer<T, Archive>::pointer_iserializer() :
    archive_pointer_iserializer<Archive>(
        * boost::serialization::type_info_implementation<T>::type::get_instance()
    ),
    m(boost::serialization::serialize_adl<Archive, T>),
    e(boost::serialization::type_info_implementation<T>::type::get_instance)
#else
BOOST_DLLEXPORT pointer_iserializer<T, Archive>::pointer_iserializer() :
    archive_pointer_iserializer<Archive>(
        * boost::serialization::type_info_implementation<T>::type::get_instance()
    )
#endif
{
    iserializer<Archive, T> & bis = iserializer<Archive, T>::instantiate();
    bis.set_bpis(this);
}

template<class Archive, class T>
struct load_non_pointer_type {
    // note this bounces the call right back to the archive
    // with no runtime overhead
    struct load_primitive {
        static void invoke(Archive & ar, T & t){
            load_access::load_primitive(ar, t);
        }
    };
    // note this bounces the call right back to the archive
    // with no runtime overhead
    struct load_only {
        static void invoke(Archive & ar, T & t){
            // short cut to user's serializer
            // make sure call is routed through the higest interface that might
            // be specialized by the user.
            boost::serialization::serialize_adl(
                ar, t, boost::serialization::version<T>::value
            );
        }
    };

    // note this save class information including version
    // and serialization level to the archive
    struct load_standard {
        static void invoke(Archive &ar, T &t){
            //BOOST_STATIC_ASSERT(! boost::is_const<T>::value);
            // borland - for some reason T is const here - even though
            // its not called that way - so fix it her
            typedef BOOST_DEDUCED_TYPENAME boost::remove_const<T>::type typex;
            void * x = & const_cast<typex &>(t);
            ar.load_object(x, iserializer<Archive, T>::instantiate());
        }
    };

    struct load_conditional {
        static void invoke(Archive &ar, T &t){
            //if(0 == (ar.get_flags() & no_tracking))
                load_standard::invoke(ar, t);
            //else
            //    load_only::invoke(ar, t);
        }
    };

    typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
            // if its primitive
            mpl::equal_to<
                boost::serialization::implementation_level<T>,
                mpl::int_<boost::serialization::primitive_type>
            >,
            mpl::identity<load_primitive>,
        // else
        BOOST_DEDUCED_TYPENAME mpl::eval_if<
        // class info / version
        mpl::greater_equal<
                    boost::serialization::implementation_level<T>,
                    mpl::int_<boost::serialization::object_class_info>
                >,
        // do standard load
        mpl::identity<load_standard>,
    // else
    BOOST_DEDUCED_TYPENAME mpl::eval_if<
        // no tracking
                mpl::equal_to<
                    boost::serialization::tracking_level<T>,
                    mpl::int_<boost::serialization::track_never>
            >,
            // do a fast load
            mpl::identity<load_only>,
        // else
        // do a fast load only tracking is turned off
        mpl::identity<load_conditional>
    > > >::type typex;

    static void invoke(Archive & ar, T &t){
        BOOST_STATIC_ASSERT((
            mpl::greater_equal<
                boost::serialization::implementation_level<T>, 
                mpl::int_<boost::serialization::primitive_type>
            >::value
        ));
        typex::invoke(ar, t);
    }
};

template<class Archive, class Tptr>
struct load_pointer_type {
    template<class T>
    struct abstract
    {
        static const basic_pointer_iserializer * register_type(Archive & /* ar */){
            #if ! defined(__BORLANDC__)
            typedef BOOST_DEDUCED_TYPENAME 
                boost::serialization::type_info_implementation<T>::type::is_polymorphic typex;
            // it has? to be polymorphic
            BOOST_STATIC_ASSERT(typex::value);
            #endif
            return static_cast<basic_pointer_iserializer *>(NULL);
         }
    };

    template<class T>
    struct non_abstract
    {
        static const basic_pointer_iserializer * register_type(Archive & ar){
            return ar.register_type(static_cast<T *>(NULL));
        }
    };

    template<class T>
    static const basic_pointer_iserializer * register_type(Archive &ar, T & /*t*/){
        // there should never be any need to load an abstract polymorphic 
        // class pointer.  Inhibiting code generation for this
        // permits abstract base classes to be used - note: exception
        // virtual serialize functions used for plug-ins
        typedef BOOST_DEDUCED_TYPENAME
            mpl::eval_if<
                serialization::is_abstract<T>,
                mpl::identity<abstract<T> >,
                mpl::identity<non_abstract<T> >    
            >::type typex;
        return typex::register_type(ar);
    }

    template<class T>
    static T * pointer_tweak(
        const boost::serialization::extended_type_info & eti,
        void * t,
        T &
    ) {
        // tweak the pointer back to the base class
        return static_cast<T *>(
            boost::serialization::void_upcast(
                eti,
                * boost::serialization::type_info_implementation<T>::type::get_instance(),
                t
            )
        );
    }

    static void invoke(Archive & ar, Tptr & t){
        const basic_pointer_iserializer * bpis_ptr = register_type(ar, *t);
        const basic_pointer_iserializer * newbpis_ptr = ar.load_pointer(
            * reinterpret_cast<void **>(&t),
            bpis_ptr,
            archive_pointer_iserializer<Archive>::find
        );
        // if the pointer isn't that of the base class
        if(newbpis_ptr != bpis_ptr){
            t = pointer_tweak(newbpis_ptr->get_eti(), t, *t);
        }
    }
};

template<class Archive, class T>
struct load_enum_type {
    static void invoke(Archive &ar, T &t){
        // convert integers to correct enum to load
        int i;
        ar >> boost::serialization::make_nvp(NULL, i);
        t = static_cast<T>(i);
    }
};

template<class Archive, class T>
struct load_array_type {
    static void invoke(Archive &ar, T &t){
        // convert integers to correct enum to load
        int current_count = sizeof(t) / (
            static_cast<char *>(static_cast<void *>(&t[1])) 
            - static_cast<char *>(static_cast<void *>(&t[0]))
        );
        int count;
        ar >> BOOST_SERIALIZATION_NVP(count);
        if(count > current_count)
            boost::throw_exception(archive::archive_exception(
                boost::archive::archive_exception::array_size_too_short
            ));
        int i;
        for(i = 0; i < count; ++i)
            ar >> boost::serialization::make_nvp("item", t[i]);
    }
};

// note bogus arguments to workaround msvc 6 silent runtime failure
template<class Archive, class T>
BOOST_DLLEXPORT 
inline const basic_pointer_iserializer &
instantiate_pointer_iserializer(
    Archive * /* ar = NULL */,
    T * /* t = NULL */
) BOOST_USED;

template<class Archive, class T>
BOOST_DLLEXPORT 
inline const basic_pointer_iserializer &
instantiate_pointer_iserializer(
    Archive * /* ar = NULL */,
    T * /* t = NULL */
){
    // note: reversal of order of arguments to work around msvc 6.0 bug
    // that manifests itself while trying to link.
    return pointer_iserializer<T, Archive>::instantiate();
}

} // detail

template<class Archive, class T>
inline void load(Archive &ar, T &t){
    // if this assertion trips. It means we're trying to load a
    // const object with a compiler that doesn't have correct
    // funtion template ordering.  On other compilers, this is
    // handled below.
    BOOST_STATIC_ASSERT(! boost::is_const<T>::value);
    typedef
        BOOST_DEDUCED_TYPENAME mpl::eval_if<is_pointer<T>,
            mpl::identity<detail::load_pointer_type<Archive, T> >
        ,//else
        BOOST_DEDUCED_TYPENAME mpl::eval_if<is_array<T>,
            mpl::identity<detail::load_array_type<Archive, T> >
        ,//else
        BOOST_DEDUCED_TYPENAME mpl::eval_if<is_enum<T>,
            mpl::identity<detail::load_enum_type<Archive, T> >
        ,//else
            mpl::identity<detail::load_non_pointer_type<Archive, T> >
        >
        >
        >::type typex;
    typex::invoke(ar, t);
}

// BORLAND
#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x560))
// borland has a couple fo problems
// a) if function is partiall specialized - see below
// const paramters are transformed to non-const ones
// b) implementation of base_object can't be made to work
// correctly which results in all base_object s being const.
// So, strip off the const for borland.  This breaks the trap
// for loading const objects - but I see no alternative
template<class Archive, class T>
inline void load(Archive &ar, const T & t){
        load(ar, const_cast<T &>(t));
}
#endif

// let wrappers through.  (Someday implement is_wrapper)
#ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
template<class Archive, class T>
inline void load(Archive &ar, const serialization::nvp<T> &t){
        boost::archive::load(ar, const_cast<serialization::nvp<T> &>(t));
}
template<class Archive>
inline void load(Archive &ar, const serialization::binary_object &t){
        boost::archive::load(ar, const_cast<serialization::binary_object &>(t));
}

//template<class Archive, class T>
//inline void load(Archive &ar, const serialization::binary_object &t){
//      load(ar, const_cast<binary_object &>(t));
//}
#endif

} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_DETAIL_ISERIALIZER_HPP
