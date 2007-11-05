// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_DETAIL_LINKED_STREAMBUF_HPP_INCLUDED
#define BOOST_IOSTREAMS_DETAIL_LINKED_STREAMBUF_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <typeinfo>
#include <boost/config.hpp>                        // member template friends.
#include <boost/iostreams/detail/char_traits.hpp>
#include <boost/iostreams/detail/ios.hpp>          // openmode.
#include <boost/iostreams/detail/streambuf.hpp>

// Must come last.
#include <boost/iostreams/detail/config/disable_warnings.hpp> // MSVC.

namespace boost { namespace iostreams { namespace detail {

template<typename Self, typename Ch, typename Tr, typename Alloc, typename Mode>
class chain_base;

template<typename Chain, typename Access, typename Mode> class chainbuf;

#define BOOST_IOSTREAMS_USING_PROTECTED_STREAMBUF_MEMBERS(base) \
    using base::eback; using base::gptr; using base::egptr; \
    using base::setg; using base::gbump; using base::pbase; \
    using base::pptr; using base::epptr; using base::setp; \
    using base::pbump; using base::underflow; using base::pbackfail; \
    using base::xsgetn; using base::overflow; using base::sputc; \
    using base::xsputn; using base::sync; using base::seekoff; \
    using base::seekpos; \
    /**/

template<typename Ch, typename Tr = BOOST_IOSTREAMS_CHAR_TRAITS(Ch) >
class linked_streambuf : public BOOST_IOSTREAMS_BASIC_STREAMBUF(Ch, Tr) {
protected:
    linked_streambuf() : true_eof_(false) { }
    void set_true_eof(bool eof) { true_eof_ = eof; }
public:

    // Should be called only after receiving an ordinary EOF indication,
    // to confirm that it represents EOF rather than WOULD_BLOCK.
    bool true_eof() const { return true_eof_; }
protected:

    //----------grant friendship to chain_base and chainbuf-------------------//

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
    template< typename Self, typename ChT, typename TrT,
              typename Alloc, typename Mode >
    friend class chain_base;
    template<typename Chain, typename Mode, typename Access>
    friend class chainbuf;
#else
public:
    typedef BOOST_IOSTREAMS_BASIC_STREAMBUF(Ch, Tr) base;
    BOOST_IOSTREAMS_USING_PROTECTED_STREAMBUF_MEMBERS(base)
#endif
    virtual void set_next(linked_streambuf<Ch, Tr>* /* next */) { }
    virtual void close(BOOST_IOS::openmode) = 0;
    virtual bool auto_close() const = 0;
    virtual void set_auto_close(bool) = 0;
    virtual bool strict_sync() = 0;
    virtual const std::type_info& component_type() const = 0;
    virtual void* component_impl() = 0;
private:
    bool true_eof_;
};

} } } // End namespaces detail, iostreams, boost.

#include <boost/iostreams/detail/config/enable_warnings.hpp> // MSVC.

#endif // #ifndef BOOST_IOSTREAMS_DETAIL_LINKED_STREAMBUF_HPP_INCLUDED
