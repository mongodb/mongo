//
// Copyright (c) Chris Glover, 2016.
//
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_TYPE_INDEX_RUNTIME_CAST_REGISTER_RUNTIME_CLASS_HPP
#define BOOST_TYPE_INDEX_RUNTIME_CAST_REGISTER_RUNTIME_CLASS_HPP

/// \file register_runtime_class.hpp
/// \brief Contains the macros BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST and
/// BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS
#include <boost/type_index.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
# pragma once
#endif

namespace boost { namespace typeindex { namespace detail {

template<typename T>
inline type_index runtime_class_construct_type_id(T const*) {
    return boost::typeindex::type_id<T>();
}

template <class Self>
constexpr const void* find_instance(boost::typeindex::type_index const&, const Self*) noexcept {
    return nullptr;
}

template <class Base, class... OtherBases, class Self>
const void* find_instance(boost::typeindex::type_index const& idx, const Self* self) noexcept {
    if (const void* ptr = self->Base::boost_type_index_find_instance_(idx)) {
        return ptr;
    }

    return boost::typeindex::detail::find_instance<OtherBases...>(idx, self);
}

}}} // namespace boost::typeindex::detail


/// \def BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS
/// \brief Macro used to make a class compatible with boost::typeindex::runtime_cast
///
/// BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS generates a virtual function
/// in the current class that, when combined with the supplied base class information, allows
/// boost::typeindex::runtime_cast to accurately convert between dynamic types of instances of
/// the current class.
///
/// BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS also adds support for boost::typeindex::type_id_runtime
/// by including BOOST_TYPE_INDEX_REGISTER_CLASS. It is typical that these features are used together,
/// but in the event that BOOST_TYPE_INDEX_REGISTER_CLASS is undesirable in the current class,
/// BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST is provided.
///
/// \b Example:
/// \code
/// struct base1 {
///     BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS()
///     virtual ~base1();
/// };
///
/// struct base2 {
///     BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS()
///     virtual ~base2();
/// };
///
/// struct derived1 : base1 {
///     BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS(base1)
/// };
///
/// struct derived2 : base1, base2 {
///     BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS(base1, base2)
/// };
///
/// ...
///
/// base1* pb1 = get_object();
/// if(derived2* pb2 = boost::typeindex::runtime_cast<derived2*>(pb1)) {
///     assert(boost::typeindex::type_id_runtime(*pb1)) == boost::typeindex::type_id<derived2>());
/// }
/// \endcode
///
/// \param base_class_seq A Boost.Preprocessor sequence of the current class' direct bases, or
/// BOOST_TYPE_INDEX_NO_BASE_CLASS if this class has no direct base classes.
#define BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS(...)                                                   \
    BOOST_TYPE_INDEX_REGISTER_CLASS                                                                               \
    BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST(__VA_ARGS__)

/// \def BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST
/// \brief Macro used to make a class compatible with boost::typeindex::runtime_cast without including
/// support for boost::typeindex::type_id_runtime.
///
/// BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST is provided as an alternative to BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS
/// in the event that support for boost::typeindex::type_id_runtime is undesirable.
///
/// \b Example:
/// \code
/// struct base1 {
///     BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST()
///     virtual ~base1();
/// };
///
/// struct base2 {
///     BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST()
///     virtual ~base2();
/// };
///
/// struct derived1 : base1 {
///     BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST(base1)
/// };
///
/// struct derived2 : base1, base2 {
///     BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST(base1, base2)
/// };
///
/// ...
///
/// base1* pb1 = get_object();
/// if(derived2* pb2 = boost::typeindex::runtime_cast<derived2*>(pb1))
/// { /* can't call boost::typeindex::type_id_runtime(*pb1) here */ }
/// \endcode
///
/// \param base_class_seq A Boost.Preprocessor sequence of the current class' direct bases, or
/// BOOST_TYPE_INDEX_NO_BASE_CLASS if this class has no direct base classes.
#define BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST(...)                                                              \
    virtual void const* boost_type_index_find_instance_(boost::typeindex::type_index const& idx) const noexcept { \
        if(idx == boost::typeindex::detail::runtime_class_construct_type_id(this))                                \
            return this;                                                                                          \
        return boost::typeindex::detail::find_instance<__VA_ARGS__>(idx, this);                                   \
    }

/// \def BOOST_TYPE_INDEX_NO_BASE_CLASS
/// \brief Instructs BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS and BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST
/// that this class has no base classes.
/// \deprecated Just remove and use BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS() or BOOST_TYPE_INDEX_IMPLEMENT_RUNTIME_CAST()
#define BOOST_TYPE_INDEX_NO_BASE_CLASS /**/

#endif // BOOST_TYPE_INDEX_RUNTIME_CAST_REGISTER_RUNTIME_CLASS_HPP
