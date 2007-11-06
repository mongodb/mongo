/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library

    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(SYMBOL_TABLE_HPP_32B0F7C6_3DD6_4113_95A5_E16516C6F45A_INCLUDED)
#define SYMBOL_TABLE_HPP_32B0F7C6_3DD6_4113_95A5_E16516C6F45A_INCLUDED

#include <map>

#include <boost/wave/wave_config.hpp>
#if BOOST_WAVE_SERIALIZATION != 0
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/map.hpp>
#endif

// this must occur after all of the includes and before any code appears
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_PREFIX
#endif

///////////////////////////////////////////////////////////////////////////////
namespace boost {
namespace wave {
namespace util {

///////////////////////////////////////////////////////////////////////////////
//
//  The symbol_table class is used for the storage of defined macros. 
//
///////////////////////////////////////////////////////////////////////////////

template <typename StringT, typename MacroDefT>
struct symbol_table 
:   public std::map<StringT, boost::shared_ptr<MacroDefT> > 
{
    typedef std::map<StringT, boost::shared_ptr<MacroDefT> > base_type;
    
    symbol_table(long uid_ = 0) 
    {}
    
#if BOOST_WAVE_SERIALIZATION != 0    
private:
    friend class boost::serialization::access;
    template<typename Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
        ar & boost::serialization::base_object<base_type>(*this);
    }
#endif
};

///////////////////////////////////////////////////////////////////////////////
}   // namespace util
}   // namespace wave
}   // namespace boost

// the suffix header occurs after all of the code
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_SUFFIX
#endif

#endif // !defined(SYMBOL_TABLE_HPP_32B0F7C6_3DD6_4113_95A5_E16516C6F45A_INCLUDED)
