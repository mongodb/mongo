// (C) Copyright 2008 CodeRage, LLC (turkanis at coderage dot com)
// (C) Copyright 2003-2007 Jonathan Turkanis
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

// To configure Boost to work with zlib, see the 
// installation instructions here:
// http://boost.org/libs/iostreams/doc/index.html?path=7

// Define BOOST_IOSTREAMS_SOURCE so that <boost/iostreams/detail/config.hpp> 
// knows that we are building the library (possibly exporting code), rather 
// than using it (possibly importing code).
#define BOOST_IOSTREAMS_SOURCE 

#include <boost/throw_exception.hpp>
#include <boost/iostreams/detail/config/dyn_link.hpp>
#include <boost/iostreams/filter/zlib.hpp> 
#include "zlib.h"   // Jean-loup Gailly's and Mark Adler's "zlib.h" header.
                    // To configure Boost to work with zlib, see the 
                    // installation instructions here:
                    // http://boost.org/libs/iostreams/doc/index.html?path=7

namespace boost { namespace iostreams {

namespace zlib {

                    // Compression levels

const int no_compression       = Z_NO_COMPRESSION;
const int best_speed           = Z_BEST_SPEED;
const int best_compression     = Z_BEST_COMPRESSION;
const int default_compression  = Z_DEFAULT_COMPRESSION;

                    // Compression methods

const int deflated             = Z_DEFLATED;

                    // Compression strategies

const int default_strategy     = Z_DEFAULT_STRATEGY;
const int filtered             = Z_FILTERED;
const int huffman_only         = Z_HUFFMAN_ONLY;

                    // Status codes

const int okay                 = Z_OK;
const int stream_end           = Z_STREAM_END;
const int stream_error         = Z_STREAM_ERROR;
const int version_error        = Z_VERSION_ERROR;
const int data_error           = Z_DATA_ERROR;
const int mem_error            = Z_MEM_ERROR;
const int buf_error            = Z_BUF_ERROR;

                    // Flush codes

const int finish               = Z_FINISH;
const int no_flush             = Z_NO_FLUSH;
const int sync_flush           = Z_SYNC_FLUSH;

                    // Code for current OS

//const int os_code              = OS_CODE;

} // End namespace zlib. 

//------------------Implementation of zlib_error------------------------------//
                    
zlib_error::zlib_error(int error) 
    : BOOST_IOSTREAMS_FAILURE("zlib error"), error_(error) 
    { }

void zlib_error::check BOOST_PREVENT_MACRO_SUBSTITUTION(int error)
{
    switch (error) {
    case Z_OK: 
    case Z_STREAM_END: 
    //case Z_BUF_ERROR: 
        return;
    case Z_MEM_ERROR: 
        boost::throw_exception(std::bad_alloc());
    default:
        boost::throw_exception(zlib_error(error));
        ;
    }
}

//------------------Implementation of zlib_base-------------------------------//

namespace detail {

zlib_base::zlib_base()
    : stream_(new z_stream), calculate_crc_(false), crc_(0), crc_imp_(0)
    { }

zlib_base::~zlib_base() { delete static_cast<z_stream*>(stream_); }

void zlib_base::before( const char*& src_begin, const char* src_end,
                        char*& dest_begin, char* dest_end )
{
    z_stream* s = static_cast<z_stream*>(stream_);
    s->next_in = reinterpret_cast<zlib::byte*>(const_cast<char*>(src_begin));
    s->avail_in = static_cast<zlib::uint>(src_end - src_begin);
    s->next_out = reinterpret_cast<zlib::byte*>(dest_begin);
    s->avail_out= static_cast<zlib::uint>(dest_end - dest_begin);
}

void zlib_base::after(const char*& src_begin, char*& dest_begin, bool compress)
{
    z_stream* s = static_cast<z_stream*>(stream_);
    const char* next_in = reinterpret_cast<const char*>(s->next_in);
    char* next_out = reinterpret_cast<char*>(s->next_out);
    if (calculate_crc_) {
        const zlib::byte* buf = compress ?
            reinterpret_cast<const zlib::byte*>(src_begin) :
            reinterpret_cast<const zlib::byte*>(
                const_cast<const char*>(dest_begin)
            );
        zlib::uint length = compress ?
            static_cast<zlib::uint>(next_in - src_begin) :
            static_cast<zlib::uint>(next_out - dest_begin);
        crc_ = crc_imp_ = crc32(crc_imp_, buf, length);
    }
    total_in_ = s->total_in;
    total_out_ = s->total_out;
    src_begin = next_in;
    dest_begin = next_out;
}

int zlib_base::xdeflate(int flush)
{ 
    return ::deflate(static_cast<z_stream*>(stream_), flush);
}

int zlib_base::xinflate(int flush)
{ 
    return ::inflate(static_cast<z_stream*>(stream_), flush);
}

void zlib_base::reset(bool compress, bool realloc)
{
    z_stream* s = static_cast<z_stream*>(stream_);
    // Undiagnosed bug:
    // deflateReset(), etc., return Z_DATA_ERROR
    //zlib_error::check BOOST_PREVENT_MACRO_SUBSTITUTION(
        realloc ?
            (compress ? deflateReset(s) : inflateReset(s)) :
            (compress ? deflateEnd(s) : inflateEnd(s))
                ;
    //);
    crc_imp_ = 0;
}

void zlib_base::do_init
    ( const zlib_params& p, bool compress, 
      zlib::xalloc_func /* alloc */, zlib::xfree_func /* free*/, 
      void* derived )
{
    calculate_crc_ = p.calculate_crc;
    z_stream* s = static_cast<z_stream*>(stream_);

    // Current interface for customizing memory management 
    // is non-conforming and has been disabled:
    //    s->zalloc = alloc;
    //    s->zfree = free;
        s->zalloc = 0;
        s->zfree = 0;
    s->opaque = derived;
    int window_bits = p.noheader? -p.window_bits : p.window_bits;
    zlib_error::check BOOST_PREVENT_MACRO_SUBSTITUTION(
        compress ?
            deflateInit2( s, 
                          p.level,
                          p.method,
                          window_bits,
                          p.mem_level,
                          p.strategy ) :
            inflateInit2(s, window_bits)
    );
}

} // End namespace detail.

//----------------------------------------------------------------------------//

} } // End namespaces iostreams, boost.
