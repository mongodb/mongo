#ifndef BOOST_SERIALIZATION_EXPORT_HPP
#define BOOST_SERIALIZATION_EXPORT_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// export.hpp: set traits of classes to be serialized

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com .
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// implementation of class export functionality.  This is an alternative to
// "forward declaration" method to provoke instantiation of derived classes
// that are to be serialized through pointers.

#include <utility>

#include <boost/config.hpp>

// if no archive headers have been included this is a no op
// this is to permit BOOST_EXPORT etc to be included in a
// file declaration header
#if ! defined(BOOST_ARCHIVE_BASIC_ARCHIVE_HPP)
#define BOOST_CLASS_EXPORT_GUID_ARCHIVE_LIST(T, K, ASEQ)

#else
#include <boost/static_assert.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/or.hpp>
#include <boost/mpl/empty.hpp>
#include <boost/mpl/front.hpp>
#include <boost/mpl/pop_front.hpp>
#include <boost/mpl/void.hpp>
#include <boost/mpl/identity.hpp>

#include <boost/archive/detail/known_archive_types.hpp>
#include <boost/serialization/force_include.hpp>
#include <boost/serialization/type_info_implementation.hpp>
#include <boost/serialization/is_abstract.hpp>

namespace boost {
namespace archive {
namespace detail {

// forward template declarations
class basic_pointer_iserializer;
template<class Archive, class T>
BOOST_DLLEXPORT const basic_pointer_iserializer &
instantiate_pointer_iserializer(Archive * ar, T *) BOOST_USED;

class basic_pointer_oserializer;
template<class Archive, class T>
BOOST_DLLEXPORT const basic_pointer_oserializer &
instantiate_pointer_oserializer(Archive * ar, T *) BOOST_USED;

namespace export_impl
{
    struct nothing{
        static void instantiate(){}
    };

    template<class Archive, class T>
    struct archive {
        struct i {
            static void invoke(){
                instantiate_pointer_iserializer(
                    static_cast<Archive *>(NULL),
                    static_cast<T *>(NULL)
                );
            }
        };
        struct o {
            static void invoke(){
                instantiate_pointer_oserializer(
                    static_cast<Archive *>(NULL),
                    static_cast<T *>(NULL)
                );
            }
        };
        static void instantiate(){
            #if defined(__GNUC__) && (__GNUC__ >= 3)
            BOOST_STATIC_ASSERT(
                Archive::is_loading::value || Archive::is_saving::value
            );
            #endif
            typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
                BOOST_DEDUCED_TYPENAME Archive::is_saving,
                mpl::identity<o>,
            // else
            BOOST_DEDUCED_TYPENAME mpl::eval_if<
                BOOST_DEDUCED_TYPENAME Archive::is_loading,
                mpl::identity<i>,
            // else
                mpl::identity<nothing>
            > >::type typex;
            typex::invoke();
        }
    };

    template<class ASeq, class T>
    struct for_each_archive {
    private:
        typedef BOOST_DEDUCED_TYPENAME mpl::pop_front<ASeq>::type tail;
        typedef BOOST_DEDUCED_TYPENAME mpl::front<ASeq>::type head;
    public:
        static void instantiate(){
            archive<head, T>::instantiate();
            typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
                mpl::empty<tail>,
                mpl::identity<nothing>,
                mpl::identity<for_each_archive<tail, T> >
            >::type typex;
            typex::instantiate();
        }
    };

} // namespace export_impl

// strictly conforming
template<class T, class ASeq>
struct export_generator {
    export_generator(){
        export_impl::for_each_archive<ASeq, T>::instantiate();
    }
    static const export_generator instance;
};

template<class T, class ASeq>
const export_generator<T, ASeq>
    export_generator<T, ASeq>::instance;

// instantiation of this template creates a static object.
template<class T>
struct guid_initializer {
    typedef BOOST_DEDUCED_TYPENAME boost::serialization::type_info_implementation<T>::type eti_type;
    static void export_register(const char *key){
        eti_type::export_register(key);
    }
    static const guid_initializer instance;
    guid_initializer(const char *key = 0) BOOST_USED ;
};

template<class T>
guid_initializer<T>::guid_initializer(const char *key){
    if(0 != key)
        export_register(key);
}

template<class T>
const guid_initializer<T> guid_initializer<T>::instance;

// only gcc seems to be able to explicitly instantiate a static instance.
// but all can instantiate a function that refers to a static instance

// the following optimization - inhibiting explicit instantiation for abstract
// classes breaks msvc compliles
template<class T, class ASeq>
struct export_instance {
    struct abstract {
        static const export_generator<T, ASeq> *
        invoke(){
            return 0;
        }
    };
    struct not_abstract {
        static const export_generator<T, ASeq> *
        invoke(){
            return & export_generator<T, ASeq>::instance;
        }
    };
};

template<class T, class ASeq>
BOOST_DLLEXPORT
std::pair<const export_generator<T, ASeq> *, const guid_initializer<T> *>
export_instance_invoke() {
    typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
        serialization::is_abstract<T>,
        mpl::identity<BOOST_DEDUCED_TYPENAME export_instance<T, ASeq>::abstract>,
        mpl::identity<BOOST_DEDUCED_TYPENAME export_instance<T, ASeq>::not_abstract>
    >::type typex;
    return std::pair<const export_generator<T, ASeq> *, const guid_initializer<T> *>(
        typex::invoke(),
        & guid_initializer<T>::instance
    );
}

template<class T, class ASeq>
struct export_archives {
    struct empty_archive_list {
        static BOOST_DLLEXPORT
        std::pair<const export_generator<T, ASeq> *, const guid_initializer<T> *>
        invoke(){
            return std::pair<const export_generator<T, ASeq> *,
                             const guid_initializer<T> *>(0, 0);
        }
    };
    struct non_empty_archive_list {
        static BOOST_DLLEXPORT
        std::pair<const export_generator<T, ASeq> *, const guid_initializer<T> *>
        invoke(){
            return export_instance_invoke<T, ASeq>();
        }
    };
};

template<class T, class ASeq>
BOOST_DLLEXPORT
std::pair<const export_generator<T, ASeq> *, const guid_initializer<T> *>
export_archives_invoke(T &, ASeq &){
    typedef BOOST_DEDUCED_TYPENAME mpl::eval_if<
        mpl::empty<ASeq>,
        mpl::identity<BOOST_DEDUCED_TYPENAME export_archives<T, ASeq>::empty_archive_list>,
        mpl::identity<BOOST_DEDUCED_TYPENAME export_archives<T, ASeq>::non_empty_archive_list>
    >::type typex;
    return typex::invoke();
}

} // namespace detail
} // namespace archive
} // namespace boost

#define BOOST_CLASS_EXPORT_GUID_ARCHIVE_LIST(T, K, ASEQ)         \
    namespace boost {                                            \
    namespace archive {                                          \
    namespace detail {                                           \
    template<>                                                   \
    const guid_initializer< T >                                  \
        guid_initializer< T >::instance(K);                      \
    template                                                     \
    BOOST_DLLEXPORT                                              \
    std::pair<const export_generator<T, ASEQ> *, const guid_initializer< T > *> \
    export_archives_invoke<T, ASEQ>(T &, ASEQ &);                \
    } } }                                                        \
    /**/

#endif

// check for unnecessary export.  T isn't polymorphic so there is no
// need to export it.
#define BOOST_CLASS_EXPORT_CHECK(T)                              \
    BOOST_STATIC_WARNING(                                        \
        boost::serialization::type_info_implementation< T >      \
            ::type::is_polymorphic::value                        \
    );                                                           \
    /**/

// the default list of archives types for which code id generated
#define BOOST_CLASS_EXPORT_GUID(T, K)                            \
    BOOST_CLASS_EXPORT_GUID_ARCHIVE_LIST(                        \
        T,                                                       \
        K,                                                       \
        boost::archive::detail::known_archive_types::type        \
    )                                                            \
    /**/

// the default exportable class identifier is the class name
#define BOOST_CLASS_EXPORT_ARCHIVE_LIST(T, ASEQ)                 \
    BOOST_CLASS_EXPORT_GUID_ARCHIVE_LIST(T, BOOST_PP_STRINGIZE(T), A)

// the default exportable class identifier is the class name
// the default list of archives types for which code id generated
// are the originally included with this serialization system
#define BOOST_CLASS_EXPORT(T)                                    \
    BOOST_CLASS_EXPORT_GUID_ARCHIVE_LIST(                        \
        T,                                                       \
        BOOST_PP_STRINGIZE(T),                                   \
        boost::archive::detail::known_archive_types::type        \
    )                                                            \
    /**/

#endif // BOOST_SERIALIZATION_EXPORT_HPP
