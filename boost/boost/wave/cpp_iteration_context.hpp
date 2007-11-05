/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library
    Definition of the preprocessor context
    
    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(CPP_ITERATION_CONTEXT_HPP_00312288_9DDB_4668_AFE5_25D3994FD095_INCLUDED)
#define CPP_ITERATION_CONTEXT_HPP_00312288_9DDB_4668_AFE5_25D3994FD095_INCLUDED

#include <iterator>
#include <fstream>
#if defined(BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS)
#include <sstream>
#endif

#include <boost/wave/wave_config.hpp>
#include <boost/wave/cpp_exceptions.hpp>
#include <boost/wave/language_support.hpp>
#include <boost/wave/util/file_position.hpp>

// this must occur after all of the includes and before any code appears
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_PREFIX
#endif

///////////////////////////////////////////////////////////////////////////////
namespace boost {
namespace wave {
namespace iteration_context_policies {

///////////////////////////////////////////////////////////////////////////////
//
//      The iteration_context_policies templates are policies for the 
//      boost::wave::iteration_context which allows to control, how a given input file 
//      is to be represented by a pair of iterators pointing to the begin and 
//      the end of the resulting input sequence.
//
///////////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////
    //
    //  load_file_to_string
    //
    //      Loads a file into a string and returns the iterators pointing to 
    //      the beginning and the end of the loaded string.
    //
    ///////////////////////////////////////////////////////////////////////////
    struct load_file_to_string 
    {
        template <typename IterContextT>
        class inner 
        {
        public:
            template <typename PositionT>
            static 
            void init_iterators(IterContextT &iter_ctx, 
                PositionT const &act_pos)
            {
                typedef typename IterContextT::iterator_type iterator_type;
                
                std::ifstream instream(iter_ctx.filename.c_str());
                if (!instream.is_open()) {
                    BOOST_WAVE_THROW(preprocess_exception, bad_include_file, 
                        iter_ctx.filename.c_str(), act_pos);
                }
                instream.unsetf(std::ios::skipws);
                
#if defined(BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS)
            // this is known to be very slow for large files on some systems
                std::copy (istream_iterator<char>(instream),
                    istream_iterator<char>(), 
                    std::inserter(iter_ctx.instring, iter_ctx.instring.end()));
#else
                iter_ctx.instring = std::string(
                    std::istreambuf_iterator<char>(instream.rdbuf()),
                    std::istreambuf_iterator<char>());
#endif // defined(BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS)

                iter_ctx.first = iterator_type(iter_ctx.instring.begin(), 
                    iter_ctx.instring.end(), PositionT(iter_ctx.filename),
                    iter_ctx.language);
                iter_ctx.last = iterator_type();
            }

        private:
            std::string instring;
        };
    };
    
///////////////////////////////////////////////////////////////////////////////
//
//  load_file
//
//      The load_file policy opens a given file and returns the wrapped
//      istreambuf_iterators.
//
///////////////////////////////////////////////////////////////////////////////
    struct load_file 
    {
        template <typename IterContextT>
        class inner {

        public:
            ~inner() { if (instream.is_open()) instream.close(); }
            
            template <typename PositionT>
            static 
            void init_iterators(IterContextT &iter_ctx, 
                PositionT const &act_pos)
            {
                typedef typename IterContextT::iterator_type iterator_type;
                
                iter_ctx.instream.open(iter_ctx.filename.c_str());
                if (!iter_ctx.instream.is_open()) {
                    BOOST_WAVE_THROW(preprocess_exception, bad_include_file, 
                        iter_ctx.filename.c_str(), act_pos);
                }
                iter_ctx.instream.unsetf(std::ios::skipws);

                using boost::spirit::make_multi_pass;
                iter_ctx.first = iterator_type(
                    make_multi_pass(std::istreambuf_iterator<char>(
                        iter_ctx.instream.rdbuf())),
                    make_multi_pass(std::istreambuf_iterator<char>()),
                    PositionT(iter_ctx.filename), iter_ctx.language);
                iter_ctx.last = iterator_type();
            }

        private:
            std::ifstream instream;
        };
    };
    
}   // namespace iteration_context_policies

///////////////////////////////////////////////////////////////////////////////
//  
template <typename IteratorT>
struct base_iteration_context 
{
public:
    base_iteration_context(
            BOOST_WAVE_STRINGTYPE const &fname, std::size_t if_block_depth = 0)   
    :   real_filename(fname), filename(fname), line(1), emitted_lines(1),
        if_block_depth(if_block_depth)
    {}
    base_iteration_context(IteratorT const &first_, IteratorT const &last_, 
            BOOST_WAVE_STRINGTYPE const &fname, std::size_t if_block_depth = 0)
    :   first(first_), last(last_), real_filename(fname), filename(fname), 
        line(1), emitted_lines(1), if_block_depth(if_block_depth)
    {}

// the actual input stream
    IteratorT first;            // actual input stream position 
    IteratorT last;             // end of input stream
    BOOST_WAVE_STRINGTYPE real_filename;  // real name of the current file
    BOOST_WAVE_STRINGTYPE filename;       // actual processed file
    unsigned int line;                    // line counter of underlying stream
    unsigned int emitted_lines;           // count of emitted newlines
    std::size_t if_block_depth; // depth of #if block recursion
};

///////////////////////////////////////////////////////////////////////////////
//  
template <
    typename IteratorT, 
    typename InputPolicyT = iteration_context_policies::load_file_to_string 
>
struct iteration_context
:   public base_iteration_context<IteratorT>,
    public InputPolicyT::template 
        inner<iteration_context<IteratorT, InputPolicyT> >
{
    typedef IteratorT iterator_type;
    typedef typename IteratorT::token_type::position_type position_type;
    
    typedef iteration_context<IteratorT, InputPolicyT> self_type;
    
    iteration_context(BOOST_WAVE_STRINGTYPE const &fname, 
            position_type const &act_pos, 
            boost::wave::language_support language_) 
    :   base_iteration_context<IteratorT>(fname), 
        language(language_)
    {
        InputPolicyT::template inner<self_type>::init_iterators(*this, act_pos);
    }
    
    boost::wave::language_support language;
};

///////////////////////////////////////////////////////////////////////////////
}   // namespace wave
}   // namespace boost

// the suffix header occurs after all of the code
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_SUFFIX
#endif

#endif // !defined(CPP_ITERATION_CONTEXT_HPP_00312288_9DDB_4668_AFE5_25D3994FD095_INCLUDED)
