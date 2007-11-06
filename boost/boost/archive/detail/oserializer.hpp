#ifndef BOOST_ARCHIVE_OSERIALIZER_HPP
#define BOOST_ARCHIVE_OSERIALIZER_HPP

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
// oserializer.hpp: interface for serialization system.

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cassert>

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/throw_exception.hpp>
#include <boost/smart_cast.hpp>
#include <boost/static_assert.hpp>
#include <boost/static_warning.hpp>

#include <boost/type_traits/is_pointer.hpp>
#include <boost/type_traits/is_fundamental.hpp>
#include <boost/type_traits/is_enum.hpp>
#include <boost/type_traits/is_volatile.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/serialization/is_abstract.hpp>

#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/and.hpp>
#include <boost/mpl/less.hpp>
#include <boost/mpl/greater_equal.hpp>
#include <boost/mpl/equal_to.hpp>
#include <boost/mpl/int.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/list.hpp>
#include <boost/mpl/empty.hpp>
#include <boost/mpl/not.hpp>

 #ifndef BOOST_SERIALIZATION_DEFAULT_TYPE_INFO   
     #include <boost/serialization/extended_type_info_typeid.hpp>   
 #endif 
// the following is need only for dynamic cast of polymorphic pointers
#include <boost/archive/detail/basic_oarchive.hpp>
#include <boost/archive/detail/basic_oserializer.hpp>
#include <boost/archive/detail/archive_pointer_oserializer.hpp>

#include <boost/serialization/force_include.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/level.hpp>
#include <boost/serialization/tracking.hpp>
#include <boost/serialization/type_info_implementation.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/void_cast.hpp>

#include <boost/archive/archive_exception.hpp>

namespace boost {

namespace serialization {
    class extended_type_info;
} // namespace serialization

namespace archive {

// an accessor to permit friend access to archives.  Needed because
// some compilers don't handle friend templates completely
class save_access {
public:
    template<class Archive>
    static void end_preamble(Archive & ar){
        ar.end_preamble();
    }
    template<class Archive, class T>
    static void save_primitive(Archive & ar, const  T & t){
        ar.end_preamble();
        ar.save(t);
    }
};

namespace detail {

template<class Archive, class T>
class oserializer : public basic_oserializer
{
private:
    // private constructor to inhibit any existence other than the 
    // static one
    explicit oserializer() :
        basic_oserializer(
            * boost::serialization::type_info_implementation<T>::type::get_instance()
        )
    {}
public:
    virtual BOOST_DLLEXPORT void save_object_data(
        basic_oarchive & ar,    
        const void *x
    ) const BOOST_USED ;
    virtual bool class_info() const {
        return boost::serialization::implementation_level<T>::value 
            >= boost::serialization::object_class_info;
    }
    virtual bool tracking(const unsigned int /* flags */) const {
//        if(0 != (flags &  no_tracking))
//            return false;
        return boost::serialization::tracking_level<T>::value == boost::serialization::track_always
            || boost::serialization::tracking_level<T>::value == boost::serialization::track_selectivly
            && serialized_as_pointer();
    }
    virtual unsigned int version() const {
        return ::boost::serialization::version<T>::value;
    }
    virtual bool is_polymorphic() const {
        typedef BOOST_DEDUCED_TYPENAME boost::serialization::type_info_implementation<
            T
        >::type::is_polymorphic::type typex;
        return typex::value;
    }
    static oserializer & instantiate(){
        static oserializer instance;
        return instance;
    }
    virtual ~oserializer(){}
};

template<class Archive, class T>
BOOST_DLLEXPORT void oserializer<Archive, T>::save_object_data(
    basic_oarchive & ar,    
    const void *x
) const {
    // make sure call is routed through the highest interface that might
    // be specialized by the user.
    boost::serialization::serialize_adl(
        boost::smart_cast_reference<Archive &>(ar),
        * static_cast<T *>(const_cast<void *>(x)),
        version()
    );
}

// instantiation of this template creates a static object.  Note inversion of
// normal argument order to workaround bizarre error in MSVC 6.0 which only
// manifests iftself during compiler time.
template<class T, class Archive>
class pointer_oserializer : public archive_pointer_oserializer<Archive> 
{
private:
    virtual const basic_oserializer & get_basic_serializer() const {
        return oserializer<Archive, T>::instantiate();
    }
    virtual BOOST_DLLEXPORT void save_object_ptr(
        basic_oarchive & ar,
        const void * x
    ) const BOOST_USED ;
#if defined(__GNUC__) || ( defined(BOOST_MSVC) && (_MSC_VER <= 1300) )
public:
#endif
    // private constructor to inhibit any existence other than the 
    // static one.  Note GCC doesn't permit constructor to be private
    explicit BOOST_DLLEXPORT pointer_oserializer() BOOST_USED;
    static const pointer_oserializer instance;
public:
    #if ! BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582))
    // at least one compiler (CW) seems to require that serialize_adl
    // be explicitly instantiated. Still under investigation. 
    void (* const m)(Archive &, T &, const unsigned);
    boost::serialization::extended_type_info * (* e)();
    #endif
    static BOOST_DLLEXPORT const pointer_oserializer & instantiate() BOOST_USED;
    virtual ~pointer_oserializer(){}
};

template<class T, class Archive>
BOOST_DLLEXPORT const pointer_oserializer<T, Archive> & 
pointer_oserializer<T, Archive>::instantiate(){
    return instance;
}

// note: instances of this template to be constructed before the main
// is called in order for things to be initialized properly.  For this
// reason, hiding the instance in a static function as was done above
// won't work here so we created a free instance here.
template<class T, class Archive>
const pointer_oserializer<T, Archive> pointer_oserializer<T, Archive>::instance;

template<class T, class Archive>
BOOST_DLLEXPORT void pointer_oserializer<T, Archive>::save_object_ptr(
    basic_oarchive & ar,
    const void * x
) const {
    assert(NULL != x);
    // make sure call is routed through the highest interface that might
    // be specialized by the user.
    T * t = static_cast<T *>(const_cast<void *>(x));
    const unsigned int file_version = boost::serialization::version<T>::value;
    Archive & ar_impl = boost::smart_cast_reference<Archive &>(ar);
    boost::serialization::save_construct_data_adl<Archive, T>(
        ar_impl, 
        t, 
        file_version
    );
    ar_impl << boost::serialization::make_nvp(NULL, * t);
}

template<class T, class Archive>
#if ! BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582))
BOOST_DLLEXPORT pointer_oserializer<T, Archive>::pointer_oserializer() :
    archive_pointer_oserializer<Archive>(
        * boost::serialization::type_info_implementation<T>::type::get_instance()
    ),
    m(boost::serialization::serialize_adl<Archive, T>),
    e(boost::serialization::type_info_implementation<T>::type::get_instance)
#else
BOOST_DLLEXPORT pointer_oserializer<T, Archive>::pointer_oserializer() :
    archive_pointer_oserializer<Archive>(
        * boost::serialization::type_info_implementation<T>::type::get_instance()
    )
#endif
{
    // make sure appropriate member function is instantiated
    oserializer<Archive, T> & bos = oserializer<Archive, T>::instantiate();
    bos.set_bpos(this);
}

template<class Archive, class T>
struct save_non_pointer_type {
    // note this bounces the call right back to the archive
    // with no runtime overhead
    struct save_primitive {
        static void invoke(Archive & ar, const T & t){
            save_access::save_primitive(ar, t);
        }
    };
    // same as above but passes through serialization
    struct save_only {
        static void invoke(Archive & ar, const T & t){
            // make sure call is routed through the highest interface that might
            // be specialized by the user.
            boost::serialization::serialize_adl(
                ar, 
                const_cast<T &>(t), 
                ::boost::serialization::version<T>::value
            );
        }
    };
    // adds class information to the archive. This includes
    // serialization level and class version
    struct save_standard {
        static void invoke(Archive &ar, const T & t){
            ar.save_object(& t, oserializer<Archive, T>::instantiate());
        }
    };

    // adds class information to the archive. This includes
    // serialization level and class version
    struct save_conditional {
        static void invoke(Archive &ar, const T &t){
            //if(0 == (ar.get_flags() & no_tracking))
                save_standard::invoke(ar, t);
            //else
            //   save_only::invoke(ar, t);
        }
    };

    typedef 
        BOOST_DEDUCED_TYPENAME mpl::eval_if<
        // if its primitive
            mpl::equal_to<
                boost::serialization::implementation_level<T>,
                mpl::int_<boost::serialization::primitive_type>
            >,
            mpl::identity<save_primitive>,
        // else
        BOOST_DEDUCED_TYPENAME mpl::eval_if<
            // class info / version
            mpl::greater_equal<
                boost::serialization::implementation_level<T>,
                mpl::int_<boost::serialization::object_class_info>
            >,
            // do standard save
            mpl::identity<save_standard>,
        // else
        BOOST_DEDUCED_TYPENAME mpl::eval_if<
                // no tracking
            mpl::equal_to<
                boost::serialization::tracking_level<T>,
                mpl::int_<boost::serialization::track_never>
            >,
            // do a fast save
            mpl::identity<save_only>,
        // else
            // do a fast save only tracking is turned off
            mpl::identity<save_conditional>
    > > >::type typex; 

    static void invoke(Archive & ar, const T & t){
        // check that we're not trying to serialize something that
        // has been marked not to be serialized.  If this your program
        // traps here, you've tried to serialize a class whose trait
        // has been marked "non-serializable". Either reset the trait
        // (see level.hpp) or change program not to serialize items of this class
        BOOST_STATIC_ASSERT((
            mpl::greater_equal<
                boost::serialization::implementation_level<T>, 
                mpl::int_<boost::serialization::primitive_type>
            >::value
        ));
        typex::invoke(ar, t);
    };
};

template<class Archive, class TPtr>
struct save_pointer_type {
    template<class T>
    struct abstract
    {
        static const basic_pointer_oserializer * register_type(Archive & /* ar */){
            // it has? to be polymorphic
            BOOST_STATIC_ASSERT(
                boost::serialization::type_info_implementation<T>::type::is_polymorphic::value
            );
            return static_cast<const basic_pointer_oserializer *>(NULL);
        }
    };

    template<class T>
    struct non_abstract
    {
        static const basic_pointer_oserializer * register_type(Archive & ar){
            return ar.register_type(static_cast<T *>(NULL));
        }
    };

    template<class T>
    static const basic_pointer_oserializer * register_type(Archive &ar, T & /*t*/){
        // there should never be any need to save an abstract polymorphic 
        // class pointer.  Inhibiting code generation for this
        // permits abstract base classes to be used - note: exception
        // virtual serialize functions used for plug-ins
        typedef 
            BOOST_DEDUCED_TYPENAME mpl::eval_if<
                serialization::is_abstract<T>,
                mpl::identity<abstract<T> >,
                mpl::identity<non_abstract<T> >       
            >::type typex;
        return typex::register_type(ar);
    }

    template<class T>
    struct non_polymorphic
    {
        static void save(
            Archive &ar, 
            const T & t, 
            const basic_pointer_oserializer * bpos_ptr
        ){
            // save the requested pointer type
            ar.save_pointer(& t, bpos_ptr);
        }
    };

    template<class T>
    struct polymorphic
    {
        static void save(
            Archive &ar, 
            const T & t, 
            const basic_pointer_oserializer * bpos_ptr
        ){
            const boost::serialization::extended_type_info * this_type
                = boost::serialization::type_info_implementation<T>::type::get_instance();
            // retrieve the true type of the object pointed to
            // if this assertion fails its an error in this library
            assert(NULL != this_type);
            const boost::serialization::extended_type_info * true_type 
                = boost::serialization::type_info_implementation<T>::type::get_derived_extended_type_info(t);
            // note:if this exception is thrown, be sure that derived pointer
            // is either regsitered or exported.
            if(NULL == true_type){
                boost::throw_exception(
                    archive_exception(archive_exception::unregistered_class)
                );
            }

            // if its not a pointer to a more derived type
            const void *vp = static_cast<const void *>(&t);
            if(*this_type == *true_type){
                ar.save_pointer(vp, bpos_ptr);
                return;
            }
            // convert pointer to more derived type. if this is thrown
            // it means that the base/derived relationship hasn't be registered
            vp = serialization::void_downcast(*true_type, *this_type, &t);
            if(NULL == vp){
                boost::throw_exception(
                    archive_exception(archive_exception::unregistered_cast)
                );
            }

            // sice true_type is valid, and this only gets made if the 
            // pointer oserializer object has been created, this should never
            // fail
            bpos_ptr = archive_pointer_oserializer<Archive>::find(* true_type);
            assert(NULL != bpos_ptr);
            if(NULL == bpos_ptr)
                boost::throw_exception(
                    archive_exception(archive_exception::unregistered_class)
                );
            ar.save_pointer(vp, bpos_ptr);
        }
    };

    template<class T>
    static void save(
        Archive & ar, 
        const T &t,
        const basic_pointer_oserializer * bpos_ptr
    ){
        typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
            BOOST_DEDUCED_TYPENAME boost::serialization::
                type_info_implementation<T>::type::is_polymorphic,
            mpl::identity<polymorphic<T> >,
            mpl::identity<non_polymorphic<T> >
        >::type typey;
        typey::save(ar, const_cast<T &>(t), bpos_ptr);
    }

    template<class T>
    static void const_check(T & t){
        BOOST_STATIC_ASSERT(! boost::is_const<T>::value);
    }

    static void invoke(Archive &ar, const TPtr t){
        #ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
            // if your program traps here, its because you tried to do
            // something like ar << t where t is a pointer to a const value
            // void f3(A const* a, text_oarchive& oa)
            // {
            //     oa << a;
            // }
            // with a compiler which doesn't support remove_const
            // const_check(* t);
        #else
            // otherwise remove the const
        #endif
        const basic_pointer_oserializer * bpos_ptr =  register_type(ar, * t);
        if(NULL == t){
            basic_oarchive & boa = boost::smart_cast_reference<basic_oarchive &>(ar);
            boa.save_null_pointer();
            save_access::end_preamble(ar);
            return;
        }
        save(ar, * t, bpos_ptr);
    };
};

template<class Archive, class T>
struct save_enum_type
{
    static void invoke(Archive &ar, const T &t){
        // convert enum to integers on save
        const int i = static_cast<int>(t);
        ar << boost::serialization::make_nvp(NULL, i);
    }
};

template<class Archive, class T>
struct save_array_type
{
    static void invoke(Archive &ar, const T &t){
        save_access::end_preamble(ar);
        // consider alignment
        int count = sizeof(t) / (
            static_cast<const char *>(static_cast<const void *>(&t[1])) 
            - static_cast<const char *>(static_cast<const void *>(&t[0]))
        );
        ar << BOOST_SERIALIZATION_NVP(count);
        int i;
        for(i = 0; i < count; ++i)
            ar << boost::serialization::make_nvp("item", t[i]);
    }
};

// note bogus arguments to workaround msvc 6 silent runtime failure
// declaration to satisfy gcc
template<class Archive, class T>
BOOST_DLLEXPORT const basic_pointer_oserializer &
instantiate_pointer_oserializer(
    Archive * /* ar = NULL */,
    T * /* t = NULL */
) BOOST_USED ;
// definition
template<class Archive, class T>
BOOST_DLLEXPORT const basic_pointer_oserializer &
instantiate_pointer_oserializer(
    Archive * /* ar = NULL */,
    T * /* t = NULL */
){
    // note: reversal of order of arguments to work around msvc 6.0 bug
    // that manifests itself while trying to link.
    return pointer_oserializer<T, Archive>::instantiate();
}

} // detail

template<class Archive, class T>
inline void save(Archive & ar, const T &t){
    typedef 
        BOOST_DEDUCED_TYPENAME mpl::eval_if<is_pointer<T>,
            mpl::identity<detail::save_pointer_type<Archive, T> >,
        //else
        BOOST_DEDUCED_TYPENAME mpl::eval_if<is_enum<T>,
            mpl::identity<detail::save_enum_type<Archive, T> >,
        //else
        BOOST_DEDUCED_TYPENAME mpl::eval_if<is_array<T>,
            mpl::identity<detail::save_array_type<Archive, T> >,
        //else
            mpl::identity<detail::save_non_pointer_type<Archive, T> >
        >
        >
        >::type typex;
    typex::invoke(ar, t);
}

#ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

template<class T>
struct check_tracking {
    typedef BOOST_DEDUCED_TYPENAME mpl::if_<
        // if its never tracked.
        BOOST_DEDUCED_TYPENAME mpl::equal_to<
            serialization::tracking_level<T>,
            mpl::int_<serialization::track_never>
        >,
        // it better not be a pointer
        mpl::not_<is_pointer<T> >,
    //else
        // otherwise if it might be tracked.  So there shouldn't
        // be any problem making a const
        is_const<T>
    >::type typex;
    BOOST_STATIC_CONSTANT(bool, value = typex::value);
};

template<class Archive, class T>
inline void save(Archive & ar, T &t){
    // if your program traps here, it indicates taht your doing one of the following:
    // a) serializing an object of a type marked "track_never" through a pointer.
    // b) saving an non-const object of a type not markd "track_never)
    // Either of these conditions may be an indicator of an error usage of the
    // serialization library and should be double checked.  See documentation on
    // object tracking.
    BOOST_STATIC_ASSERT(check_tracking<T>::value);
        save(ar, const_cast<const T &>(t));
}
#endif

} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_OSERIALIZER_HPP
