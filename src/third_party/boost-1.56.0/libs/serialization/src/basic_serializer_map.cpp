/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// serializer_map.cpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
#  pragma warning (disable : 4786) // too long name, harmless warning
#endif

#include <set>
#include <utility>

#define BOOST_ARCHIVE_SOURCE
// include this to prevent linker errors when the
// same modules are marked export and import.
#define BOOST_SERIALIZATION_SOURCE

#include <boost/archive/archive_exception.hpp>
#include <boost/serialization/throw_exception.hpp>

#include <boost/archive/detail/basic_serializer.hpp>
#include <boost/archive/detail/basic_serializer_map.hpp>

namespace boost {
    namespace serialization {
        class extended_type_info;
    }
namespace archive {
namespace detail {

bool  
basic_serializer_map::type_info_pointer_compare::operator()(
    const basic_serializer * lhs, const basic_serializer * rhs
) const {
    return *lhs < *rhs;
}

BOOST_ARCHIVE_DECL(bool) 
basic_serializer_map::insert(const basic_serializer * bs){
    // attempt to insert serializer into it's map
    // the following is commented out - rather than being just
    // deleted as a reminder not to try this.

    // const std::pair<map_type::iterator, bool> result =
        m_map.insert(bs);

    // At first it seemed like a good idea.  It enforced the
    // idea that a type be exported from at most one code module
    // (DLL or mainline).  This would enforce a "one definition rule" 
    // across code modules. This seems a good idea to me.  
    // But it seems that it's just too hard for many users to implement.

    // Ideally, I would like to make this exception a warning -
    // but there isn't anyway to do that.

    // if this fails, it's because it's been instantiated
    // in multiple modules - DLLS - a recipe for problems.
    // So trap this here
    // if(!result.second){
    //     boost::serialization::throw_exception(
    //         archive_exception(
    //             archive_exception::multiple_code_instantiation,
    //             bs->get_debug_info()
    //         )
    //     );
    // }
    return true;
}

BOOST_ARCHIVE_DECL(void) 
basic_serializer_map::erase(const basic_serializer * bs){
    map_type::iterator it = m_map.begin();
    map_type::iterator it_end = m_map.end();

    while(it != it_end){
        // note item 9 from Effective STL !!! it++
        if(*it == bs)
            m_map.erase(it++);
        else
            it++;
    }
    // note: we can't do this since some of the eti records
    // we're pointing to might be expired and the comparison
    // won't work.  Leave this as a reminder not to "optimize" this.
    //it = m_map.find(bs);
    //assert(it != m_map.end());
    //if(*it == bs)
    //    m_map.erase(it);
}
BOOST_ARCHIVE_DECL(const basic_serializer *)
basic_serializer_map::find(
    const boost::serialization::extended_type_info & eti
) const {
    const basic_serializer_arg bs(eti);
    map_type::const_iterator it;
    it = m_map.find(& bs);
    if(it == m_map.end()){
        BOOST_ASSERT(false);
        return 0;
    }
    return *it;
}

} // namespace detail
} // namespace archive
} // namespace boost

