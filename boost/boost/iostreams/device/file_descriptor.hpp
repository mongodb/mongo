// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

// Inspired by fdstream.hpp, (C) Copyright Nicolai M. Josuttis 2001,
// available at http://www.josuttis.com/cppcode/fdstream.html.

#ifndef BOOST_IOSTREAMS_FILE_DESCRIPTOR_HPP_INCLUDED
#define BOOST_IOSTREAMS_FILE_DESCRIPTOR_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <string>                          // file pathnames.
#include <boost/cstdint.hpp>               // intmax_t.
#include <boost/iostreams/categories.hpp>  // tags.
#include <boost/iostreams/detail/config/auto_link.hpp>
#include <boost/iostreams/detail/config/dyn_link.hpp>
#include <boost/iostreams/detail/config/windows_posix.hpp>
#include <boost/iostreams/detail/ios.hpp>  // openmode, seekdir, int types.
#include <boost/iostreams/positioning.hpp>
#include <boost/shared_ptr.hpp>

// Must come last.
#include <boost/config/abi_prefix.hpp>

namespace boost { namespace iostreams {

class BOOST_IOSTREAMS_DECL file_descriptor {
public:
#ifdef BOOST_IOSTREAMS_WINDOWS
    typedef void*  handle_type;
#endif
    typedef char   char_type;
    struct category
        : seekable_device_tag,
          closable_tag
        { };
    file_descriptor() : pimpl_(new impl) { }
    explicit file_descriptor(int fd, bool close_on_exit = false)
        : pimpl_(new impl(fd, close_on_exit))
        { }
#ifdef BOOST_IOSTREAMS_WINDOWS
    explicit file_descriptor(handle_type handle, bool close_on_exit = false)
        : pimpl_(new impl(handle, close_on_exit))
        { }
#endif
    explicit file_descriptor( const std::string& path,
                              BOOST_IOS::openmode mode =
                                  BOOST_IOS::in | BOOST_IOS::out,
                              BOOST_IOS::openmode base_mode =
                                  BOOST_IOS::in | BOOST_IOS::out )
        : pimpl_(new impl)
    { open(path, mode, base_mode); }
    void open( const std::string& path,
               BOOST_IOS::openmode =
                   BOOST_IOS::in | BOOST_IOS::out,
               BOOST_IOS::openmode base_mode =
                   BOOST_IOS::in | BOOST_IOS::out );
    bool is_open() const { return pimpl_->flags_ != 0; }
    std::streamsize read(char_type* s, std::streamsize n);
    std::streamsize write(const char_type* s, std::streamsize n);
    std::streampos seek(stream_offset off, BOOST_IOS::seekdir way);
    void close();
private:
    struct impl {
        impl() : fd_(-1), flags_(0) { }
        impl(int fd, bool close_on_exit)
            : fd_(fd), flags_(0)
        { if (close_on_exit) flags_ |= impl::close_on_exit; }
    #ifdef BOOST_IOSTREAMS_WINDOWS
        impl(handle_type handle, bool close_on_exit)
            : handle_(handle), flags_(has_handle)
        { if (close_on_exit) flags_ |= impl::close_on_exit; }
    #endif
        ~impl() {
            if (flags_ & close_on_exit) close_impl(*this);
        }
        enum flags {
            close_on_exit = 1,
            has_handle = 2,
            append = 4
        };
        int          fd_;
    #ifdef BOOST_IOSTREAMS_WINDOWS
        handle_type  handle_;
    #endif
        int          flags_;
    };
    friend struct impl;

    static void close_impl(impl&);

    shared_ptr<impl> pimpl_;
};

struct file_descriptor_source : private file_descriptor {
#ifdef BOOST_IOSTREAMS_WINDOWS
    typedef void*  handle_type;
#endif
    typedef char   char_type;
    struct category : public source_tag, closable_tag { };
    using file_descriptor::read;
    using file_descriptor::open;
    using file_descriptor::is_open;
    using file_descriptor::close;
    file_descriptor_source() { }
    explicit file_descriptor_source(int fd, bool close_on_exit = false)
        : file_descriptor(fd, close_on_exit)
        { }
#ifdef BOOST_IOSTREAMS_WINDOWS
    explicit file_descriptor_source( handle_type handle,
                                     bool close_on_exit = false )
        : file_descriptor(handle, close_on_exit)
        { }
#endif
    explicit file_descriptor_source( const std::string& path,
                                     BOOST_IOS::openmode m = BOOST_IOS::in )
        : file_descriptor(path, m & ~BOOST_IOS::out, BOOST_IOS::in)
        { }
};

struct file_descriptor_sink : private file_descriptor {
#ifdef BOOST_IOSTREAMS_WINDOWS
    typedef void*  handle_type;
#endif
    typedef char   char_type;
    struct category : public sink_tag, closable_tag { };
    using file_descriptor::write;
    using file_descriptor::open;
    using file_descriptor::is_open;
    using file_descriptor::close;
    file_descriptor_sink() { }
    explicit file_descriptor_sink(int fd, bool close_on_exit = false)
        : file_descriptor(fd, close_on_exit)
        { }
#ifdef BOOST_IOSTREAMS_WINDOWS
    explicit file_descriptor_sink( handle_type handle,
                                   bool close_on_exit = false )
        : file_descriptor(handle, close_on_exit)
        { }
#endif
    explicit file_descriptor_sink( const std::string& path,
                                   BOOST_IOS::openmode m = BOOST_IOS::out )
        : file_descriptor(path, m & ~BOOST_IOS::in, BOOST_IOS::out)
        { }
};

} } // End namespaces iostreams, boost.

#include <boost/config/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif // #ifndef BOOST_IOSTREAMS_FILE_DESCRIPTOR_HPP_INCLUDED
