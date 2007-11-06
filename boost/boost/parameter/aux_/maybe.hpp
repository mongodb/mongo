// Copyright Daniel Wallin 2006. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PARAMETER_MAYBE_060211_HPP
# define BOOST_PARAMETER_MAYBE_060211_HPP

# include <boost/mpl/if.hpp>
# include <boost/mpl/identity.hpp>
# include <boost/type_traits/is_reference.hpp>
# include <boost/type_traits/add_reference.hpp>
# include <boost/optional.hpp>
# include <boost/python/detail/referent_storage.hpp>
# include <boost/type_traits/remove_cv.hpp>
# include <boost/type_traits/add_const.hpp>

namespace boost { namespace parameter { namespace aux {

struct maybe_base {};

template <class T>
struct maybe : maybe_base
{
    typedef typename add_reference<
# if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
        T const
# else 
        typename add_const<T>::type
# endif 
    >::type reference;
    
    typedef typename remove_cv<
        BOOST_DEDUCED_TYPENAME remove_reference<reference>::type
    >::type non_cv_value;
        
    explicit maybe(T value)
      : value(value)
      , constructed(false)
    {}

    maybe()
      : constructed(false)
    {}

    ~maybe()
    {
        if (constructed)
            this->destroy();
    }

    reference construct(reference value) const
    {
        return value;
    }

    template <class U>
    reference construct2(U const& value) const
    {
        new (m_storage.bytes) non_cv_value(value);
        constructed = true;
        return *(non_cv_value*)m_storage.bytes;
    }

    template <class U>
    reference construct(U const& value) const
    {
        return this->construct2(value);
    }

    void destroy()
    {
        ((non_cv_value*)m_storage.bytes)->~non_cv_value();
    }

    typedef reference(maybe<T>::*safe_bool)() const;

    operator safe_bool() const
    {
        return value ? &maybe<T>::get : 0 ;
    }

    reference get() const
    {
        return value.get();
    }

private:
    boost::optional<T> value;
    mutable bool constructed;
    mutable typename boost::python::detail::referent_storage<
        reference
    >::type m_storage;
};

}}} // namespace boost::parameter::aux

#endif // BOOST_PARAMETER_MAYBE_060211_HPP

