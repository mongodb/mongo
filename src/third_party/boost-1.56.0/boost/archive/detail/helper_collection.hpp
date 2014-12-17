#ifndef BOOST_ARCHIVE_DETAIL_HELPER_COLLECTION_HPP
#define BOOST_ARCHIVE_DETAIL_HELPER_COLLECTION_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// helper_collection.hpp: archive support for run-time helpers

// (C) Copyright 2002-2008 Robert Ramey and Joaquin M Lopez Munoz
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cstddef> // NULL
#include <typeinfo>
#include <vector>
#include <utility>
#include <memory>
#include <algorithm>

#include <boost/config.hpp>

#ifdef BOOST_NO_CXX11_SMART_PTR
    #include <boost/smart_ptr/shared_ptr.hpp>
    #include <boost/smart_ptr/make_shared.hpp>
#endif

namespace boost {

namespace archive {
namespace detail {

class helper_collection
{
    helper_collection(const helper_collection&);              // non-copyable
    helper_collection& operator = (const helper_collection&); // non-copyable

    // note: we dont' actually "share" the function object pointer
    // we only use shared_ptr to make sure that it get's deleted

    #ifndef BOOST_NO_CXX11_SMART_PTR
        typedef std::pair<
            const std::type_info *,
            std::shared_ptr<void>
        > helper_value_type;
        template<class T>
        std::shared_ptr<void> make_helper_ptr(){
            return std::make_shared<T>();
        }
    #else
        typedef std::pair<
            const std::type_info *,
            boost::shared_ptr<void>
        > helper_value_type;
        template<class T>
        boost::shared_ptr<void> make_helper_ptr(){
            return boost::make_shared<T>();
        }
    #endif
    typedef std::vector<helper_value_type> collection;
    collection m_collection;

    struct predicate {
        const std::type_info * m_ti;
        bool operator()(helper_value_type const &rhs) const {
            return *m_ti == *rhs.first;
        }
        predicate(const std::type_info * ti) :
            m_ti(ti)
        {}
    };
protected:
    helper_collection(){}
    ~helper_collection(){}
public:
    template<typename Helper>
    Helper& get_helper(Helper * = NULL) {

        const std::type_info * eti = & typeid(Helper);

        collection::const_iterator it =
            std::find_if(
                m_collection.begin(),
                m_collection.end(),
                predicate(eti)
            );

        void * rval;
        if(it == m_collection.end()){
            m_collection.push_back(
                std::make_pair(eti, make_helper_ptr<Helper>())
            );
            rval = m_collection.back().second.get();
        }
        else{
            rval = it->second.get();
        }
        return *static_cast<Helper *>(rval);
    }
};

} // namespace detail
} // namespace serialization
} // namespace boost

#endif // BOOST_ARCHIVE_DETAIL_HELPER_COLLECTION_HPP
