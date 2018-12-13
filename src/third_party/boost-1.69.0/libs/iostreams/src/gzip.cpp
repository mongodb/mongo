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

#include <boost/iostreams/detail/config/dyn_link.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/throw_exception.hpp>

namespace boost { namespace iostreams {

//------------------Implementation of gzip_header-----------------------------//

namespace detail {

void gzip_header::process(char c)
{
    uint8_t value = static_cast<uint8_t>(c);
    switch (state_) {
    case s_id1:
        if (value != gzip::magic::id1)
            boost::throw_exception(gzip_error(gzip::bad_header));
        state_ = s_id2;
        break;
    case s_id2:
        if (value != gzip::magic::id2)
            boost::throw_exception(gzip_error(gzip::bad_header));
        state_ = s_cm;
        break;
    case s_cm:
        if (value != gzip::method::deflate)
            boost::throw_exception(gzip_error(gzip::bad_method));
        state_ = s_flg;
        break;
    case s_flg:
        flags_ = value;
        state_ = s_mtime;
        break;
    case s_mtime:
        mtime_ += value << (offset_ * 8);
        if (offset_ == 3) {
            state_ = s_xfl;
            offset_ = 0;
        } else {
            ++offset_;
        }
        break;
    case s_xfl:
        state_ = s_os;
        break;
    case s_os:
        os_ = value;
        if (flags_ & gzip::flags::extra) {
            state_ = s_xlen;
        } else if (flags_ & gzip::flags::name) {
            state_ = s_name;
        } else if (flags_ & gzip::flags::comment) {
            state_ = s_comment;
        } else if (flags_ & gzip::flags::header_crc) {
            state_ = s_hcrc;
        } else {
            state_ = s_done;
        }
        break;
    case s_xlen:
        xlen_ += value << (offset_ * 8);
        if (offset_ == 1) {
            state_ = s_extra;
            offset_ = 0;
        } else {
            ++offset_;
        }
        break;
    case s_extra:
        if (--xlen_ == 0) {
            if (flags_ & gzip::flags::name) {
                state_ = s_name;
            } else if (flags_ & gzip::flags::comment) {
                state_ = s_comment;
            } else if (flags_ & gzip::flags::header_crc) {
                state_ = s_hcrc;
            } else {
                state_ = s_done;
            }
        }
        break;
    case s_name:
        if (c != 0) {
            file_name_ += c;
        } else if (flags_ & gzip::flags::comment) {
            state_ = s_comment;
        } else if (flags_ & gzip::flags::header_crc) {
            state_ = s_hcrc;
        } else {
            state_ = s_done;
        }
        break;
    case s_comment:
        if (c != 0) {
            comment_ += c;
        } else if (flags_ & gzip::flags::header_crc) {
            state_ = s_hcrc;
        } else {
            state_ = s_done;
        }
        break;
    case s_hcrc:
        if (offset_ == 1) {
            state_ = s_done;
            offset_ = 0;
        } else {
            ++offset_;
        }
        break;
    default:
        BOOST_ASSERT(0);
    }
}

void gzip_header::reset()
{
    file_name_.clear();
    comment_.clear();
    os_ = flags_ = offset_ = xlen_ = 0;
    mtime_ = 0;
    state_ = s_id1;
}

//------------------Implementation of gzip_footer-----------------------------//

void gzip_footer::process(char c)
{
    uint8_t value = static_cast<uint8_t>(c);
    if (state_ == s_crc) {
        crc_ += value << (offset_ * 8);
        if (offset_ == 3) {
            state_ = s_isize;
            offset_ = 0;
        } else {
            ++offset_;
        }
    } else if (state_ == s_isize) {
        isize_ += value << (offset_ * 8);
        if (offset_ == 3) {
            state_ = s_done;
            offset_ = 0;
        } else {
            ++offset_;
        }
    } else {
        BOOST_ASSERT(0);
    }
}

void gzip_footer::reset()
{
    crc_ = isize_ = offset_ = 0;
    state_ = s_crc;
}

} // End namespace boost::iostreams::detail.

} } // End namespaces iostreams, boost.
