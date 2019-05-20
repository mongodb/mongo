// (C) Copyright 2008 CodeRage, LLC (turkanis at coderage dot com)
// (C) Copyright 2003-2007 Jonathan Turkanis
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

// To configure Boost to work with libbz2, see the 
// installation instructions here:
// http://boost.org/libs/iostreams/doc/index.html?path=7

// Define BOOST_IOSTREAMS_SOURCE so that <boost/iostreams/detail/config.hpp> 
// knows that we are building the library (possibly exporting code), rather 
// than using it (possibly importing code).
#define BOOST_IOSTREAMS_SOURCE 

#include <boost/throw_exception.hpp>
#include <boost/iostreams/detail/config/dyn_link.hpp>
#include <boost/iostreams/filter/bzip2.hpp> 
#include "bzlib.h"  // Julian Seward's "bzip.h" header.
                    // To configure Boost to work with libbz2, see the 
                    // installation instructions here:
                    // http://boost.org/libs/iostreams/doc/index.html?path=7
                    
namespace boost { namespace iostreams {

namespace bzip2 {

                    // Status codes

const int ok                   = BZ_OK;
const int run_ok               = BZ_RUN_OK;
const int flush_ok             = BZ_FLUSH_OK;
const int finish_ok            = BZ_FINISH_OK;
const int stream_end           = BZ_STREAM_END;
const int sequence_error       = BZ_SEQUENCE_ERROR;
const int param_error          = BZ_PARAM_ERROR;
const int mem_error            = BZ_MEM_ERROR;
const int data_error           = BZ_DATA_ERROR;
const int data_error_magic     = BZ_DATA_ERROR_MAGIC;
const int io_error             = BZ_IO_ERROR;
const int unexpected_eof       = BZ_UNEXPECTED_EOF;
const int outbuff_full         = BZ_OUTBUFF_FULL;
const int config_error         = BZ_CONFIG_ERROR;

                    // Action codes

const int finish               = BZ_FINISH;
const int run                  = BZ_RUN;

} // End namespace bzip2. 

//------------------Implementation of bzip2_error-----------------------------//
                    
bzip2_error::bzip2_error(int error)
    : BOOST_IOSTREAMS_FAILURE("bzip2 error"), error_(error) 
    { }

void bzip2_error::check BOOST_PREVENT_MACRO_SUBSTITUTION(int error)
{
    switch (error) {
    case BZ_OK: 
    case BZ_RUN_OK: 
    case BZ_FLUSH_OK:
    case BZ_FINISH_OK:
    case BZ_STREAM_END:
        return;
    case BZ_MEM_ERROR: 
        boost::throw_exception(std::bad_alloc());
    default:
        boost::throw_exception(bzip2_error(error));
    }
}

//------------------Implementation of bzip2_base------------------------------//

namespace detail {

bzip2_base::bzip2_base(const bzip2_params& params)
    : params_(params), stream_(new bz_stream), ready_(false) 
    { }

bzip2_base::~bzip2_base() { delete static_cast<bz_stream*>(stream_); }

void bzip2_base::before( const char*& src_begin, const char* src_end,
                         char*& dest_begin, char* dest_end )
{
    bz_stream* s = static_cast<bz_stream*>(stream_);
    s->next_in = const_cast<char*>(src_begin);
    s->avail_in = static_cast<unsigned>(src_end - src_begin);
    s->next_out = reinterpret_cast<char*>(dest_begin);
    s->avail_out= static_cast<unsigned>(dest_end - dest_begin);
}

void bzip2_base::after(const char*& src_begin, char*& dest_begin)
{
    bz_stream* s = static_cast<bz_stream*>(stream_);
    src_begin = const_cast<char*>(s->next_in);
    dest_begin = s->next_out;
}

int bzip2_base::check_end(const char* src_begin, const char* dest_begin)
{
    bz_stream* s = static_cast<bz_stream*>(stream_);
    if( src_begin == s->next_in &&
        s->avail_in == 0 &&
        dest_begin == s->next_out) {
        return bzip2::unexpected_eof;
    } else {
        return bzip2::ok;
    }
}

int bzip2_base::end(bool compress, std::nothrow_t)
{
    if (!ready_) return bzip2::ok;
    ready_ = false;
    bz_stream* s = static_cast<bz_stream*>(stream_);
    return compress ? BZ2_bzCompressEnd(s) :  BZ2_bzDecompressEnd(s);
}

void bzip2_base::end(bool compress)
{
    bzip2_error::check BOOST_PREVENT_MACRO_SUBSTITUTION(
        end(compress, std::nothrow)
    );
}

int bzip2_base::compress(int action)
{
    return BZ2_bzCompress(static_cast<bz_stream*>(stream_), action);
}

int bzip2_base::decompress()
{
    return BZ2_bzDecompress(static_cast<bz_stream*>(stream_));
}

void bzip2_base::do_init
    ( bool compress, 
      bzip2::alloc_func /* alloc */, 
      bzip2::free_func /* free */, 
      void* derived )
{
    bz_stream* s = static_cast<bz_stream*>(stream_);

    // Current interface for customizing memory management 
    // is non-conforming and has been disabled:
    //    s->bzalloc = alloc;
    //    s->bzfree = free;
        s->bzalloc = 0;
        s->bzfree = 0;
    s->opaque = derived;
    bzip2_error::check BOOST_PREVENT_MACRO_SUBSTITUTION( 
        compress ?
            BZ2_bzCompressInit( s,
                                params_.block_size, 
                                0, 
                                params_.work_factor ) :
            BZ2_bzDecompressInit( s, 
                                  0, 
                                  params_.small )
    );
    ready_ = true;
}

} // End namespace detail.

//----------------------------------------------------------------------------//

} } // End namespaces iostreams, boost.
