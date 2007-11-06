// (C) Copyright Jonathan Turkanis 2005.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_RESTRICT_HPP_INCLUDED
#define BOOST_IOSTREAMS_RESTRICT_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <algorithm>          // min.
#include <utility>            // pair.
#include <boost/cstdint.hpp>  // intmax_t.
#include <boost/config.hpp>   // DEDUCED_TYPENAME.
#include <boost/iostreams/categories.hpp>
#include <boost/iostreams/char_traits.hpp>
#include <boost/iostreams/detail/adapter/basic_adapter.hpp>
#include <boost/iostreams/detail/call_traits.hpp>
#include <boost/iostreams/detail/enable_if_stream.hpp>
#include <boost/iostreams/detail/error.hpp>
#include <boost/iostreams/detail/ios.hpp>     // failure.
#include <boost/iostreams/detail/select.hpp>
#include <boost/iostreams/operations.hpp>
#include <boost/iostreams/skip.hpp>
#include <boost/iostreams/traits.hpp>         // mode_of, is_direct.
#include <boost/mpl/bool.hpp>
#include <boost/static_assert.hpp>
#include <boost/type_traits/is_convertible.hpp>

#include <boost/iostreams/detail/config/disable_warnings.hpp> // VC7.1 C4244.

namespace boost { namespace iostreams {

namespace detail {

//
// Template name: restricted_indirect_device.
// Description: Provides an restricted view of an indirect Device.
// Template paramters:
//      Device - An indirect model of Device that models either Source or
//          SeekableDevice.
//
template<typename Device>
class restricted_indirect_device : public basic_adapter<Device> {
private:
    typedef typename detail::param_type<Device>::type  param_type;
public:
    typedef typename char_type_of<Device>::type        char_type;
    struct category
        : mode_of<Device>::type,
          device_tag,
          closable_tag,
          flushable_tag,
          localizable_tag,
          optimally_buffered_tag
        { };
    restricted_indirect_device( param_type dev, stream_offset off,
                                stream_offset len = -1 );
    std::streamsize read(char_type* s, std::streamsize n);
    std::streamsize write(const char_type* s, std::streamsize n);
    std::streampos seek(stream_offset off, BOOST_IOS::seekdir way);
private:
    stream_offset beg_, pos_, end_;
};

//
// Template name: restricted_direct_device.
// Description: Provides an restricted view of a Direct Device.
// Template paramters:
//      Device - A model of Direct and Device.
//
template<typename Device>
class restricted_direct_device : public basic_adapter<Device> {
public:
    typedef typename char_type_of<Device>::type  char_type;
    typedef std::pair<char_type*, char_type*>    pair_type;
    struct category
        : mode_of<Device>::type,
          device_tag,
          direct_tag,
          closable_tag,
          localizable_tag
        { };
    restricted_direct_device( const Device& dev, stream_offset off,
                              stream_offset len = -1 );
    pair_type input_sequence();
    pair_type output_sequence();
private:
    pair_type sequence(mpl::true_);
    pair_type sequence(mpl::false_);
    char_type *beg_, *end_;
};

//
// Template name: restricted_filter.
// Description: Provides an restricted view of a Filter.
// Template paramters:
//      Filter - An indirect model of Filter.
//
template<typename Filter>
class restricted_filter : public basic_adapter<Filter> {
public:
    typedef typename char_type_of<Filter>::type char_type;
    struct category
        : mode_of<Filter>::type,
          filter_tag,
          multichar_tag,
          closable_tag,
          localizable_tag,
          optimally_buffered_tag
        { };
    restricted_filter( const Filter& flt, stream_offset off, 
                       stream_offset len = -1 );

    template<typename Source>
    std::streamsize read(Source& src, char_type* s, std::streamsize n)
    {
        using namespace std;
        if (!open_)
            open(src);
        streamsize amt =
            end_ != -1 ?
                (std::min) (n, static_cast<streamsize>(end_ - pos_)) :
                n;
        streamsize result = iostreams::read(this->component(), src, s, amt);
        if (result != -1)
            pos_ += result;
        return result;
    }

    template<typename Sink>
    std::streamsize write(Sink& snk, const char_type* s, std::streamsize n)
    {
        if (!open_)
            open(snk);
        if (end_ != -1 && pos_ + n >= end_)
            bad_write();
        std::streamsize result = 
            iostreams::write(this->component(), snk, s, n);
        pos_ += result;
        return result;
    }

    template<typename Device>
    std::streampos seek(Device& dev, stream_offset off, BOOST_IOS::seekdir way)
    {
        stream_offset next;
        if (way == BOOST_IOS::beg) {
            next = beg_ + off;
        } else if (way == BOOST_IOS::cur) {
            next = pos_ + off;
        } else if (end_ != -1) {
            next = end_ + off;
        } else {
            // Restriction is half-open; seek relative to the actual end.
            pos_ = this->component().seek(dev, off, BOOST_IOS::end);
            if (pos_ < beg_)
                bad_seek();
            return offset_to_position(pos_ - beg_);
        }
        if (next < beg_ || end_ != -1 && next >= end_)
            bad_seek();
        pos_ = this->component().seek(dev, next, BOOST_IOS::cur);
        return offset_to_position(pos_ - beg_);
    }
private:
    template<typename Device>
    void open(Device& dev)
    {
        open_ = true;
        iostreams::skip(this->component(), dev, beg_);
    }
    stream_offset  beg_, pos_, end_;
    bool           open_;
};

template<typename T>
struct restriction_traits
    : iostreams::select<  // Disambiguation for Tru64.
          is_filter<T>,  restricted_filter<T>,
          is_direct<T>,  restricted_direct_device<T>,
          else_,         restricted_indirect_device<T>
      >
    { };

} // End namespace detail.

template<typename T>
struct restriction : public detail::restriction_traits<T>::type {
    typedef typename detail::param_type<T>::type          param_type;
    typedef typename detail::restriction_traits<T>::type  base_type;
    restriction(param_type t, stream_offset off, stream_offset len = -1)
        : base_type(t, off, len)
        { }
};

//--------------Implementation of restrict------------------------------------//

// Note: The following workarounds are patterned after resolve.hpp. It has not
// yet been confirmed that they are necessary.

#ifndef BOOST_IOSTREAMS_BROKEN_OVERLOAD_RESOLUTION //-------------------------//
# ifndef BOOST_IOSTREAMS_NO_STREAM_TEMPLATES //-------------------------------//

template<typename T>
restriction<T> restrict( const T& t, stream_offset off, stream_offset len = -1
                         BOOST_IOSTREAMS_DISABLE_IF_STREAM(T) )
{ return restriction<T>(t, off, len); }

template<typename Ch, typename Tr>
restriction< std::basic_streambuf<Ch, Tr> >
restrict(std::basic_streambuf<Ch, Tr>& sb, stream_offset off, stream_offset len = -1)
{ return restriction< std::basic_streambuf<Ch, Tr> >(sb, off, len); }

template<typename Ch, typename Tr>
restriction< std::basic_istream<Ch, Tr> >
restrict(std::basic_istream<Ch, Tr>& is, stream_offset off, stream_offset len = -1)
{ return restriction< std::basic_istream<Ch, Tr> >(is, off, len); }

template<typename Ch, typename Tr>
restriction< std::basic_ostream<Ch, Tr> >
restrict(std::basic_ostream<Ch, Tr>& os, stream_offset off, stream_offset len = -1)
{ return restriction< std::basic_ostream<Ch, Tr> >(os, off, len); }

template<typename Ch, typename Tr>
restriction< std::basic_iostream<Ch, Tr> >
restrict(std::basic_iostream<Ch, Tr>& io, stream_offset off, stream_offset len = -1)
{ return restriction< std::basic_iostream<Ch, Tr> >(io, off, len); }

# else // # ifndef BOOST_IOSTREAMS_NO_STREAM_TEMPLATES //---------------------//

template<typename T>
restriction<T> restrict( const T& t, stream_offset off, stream_offset len = -1
                         BOOST_IOSTREAMS_DISABLE_IF_STREAM(T) )
{ return restriction<T>(t, off, len); }

restriction<std::streambuf> 
restrict(std::streambuf& sb, stream_offset off, stream_offset len = -1)
{ return restriction<std::streambuf>(sb, off, len); }

restriction<std::istream> 
restrict(std::istream<Ch, Tr>& is, stream_offset off, stream_offset len = -1)
{ return restriction<std::istream>(is, off, len); }

restriction<std::ostream> 
restrict(std::ostream<Ch, Tr>& os, stream_offset off, stream_offset len = -1)
{ return restriction<std::ostream>(os, off, len); }

restriction<std::iostream> 
restrict(std::iostream<Ch, Tr>& io, stream_offset off, stream_offset len = -1)
{ return restriction<std::iostream>(io, off, len); }

# endif // # ifndef BOOST_IOSTREAMS_NO_STREAM_TEMPLATES //--------------------//
#else // #ifndef BOOST_IOSTREAMS_BROKEN_OVERLOAD_RESOLUTION //----------------//

template<typename T>
restriction<T> 
restrict(const T& t, stream_offset off, stream_offset len, mpl::true_)
{   // Bad overload resolution.
    return restriction<T>(const_cast<T&>(t, off, len));
}

template<typename T>
restriction<T> 
restrict(const T& t, stream_offset off, stream_offset len, mpl::false_)
{ return restriction<T>(t, off, len); }

template<typename T>
restriction<T> 
restrict( const T& t, stream_offset off, stream_offset len = -1
          BOOST_IOSTREAMS_DISABLE_IF_STREAM(T) )
{ return restrict(t, off, len, is_std_io<T>()); }

# if !BOOST_WORKAROUND(__BORLANDC__, < 0x600) && \
     !BOOST_WORKAROUND(BOOST_MSVC, <= 1300) && \
     !defined(__GNUC__) // ---------------------------------------------------//

template<typename T>
restriction<T>
restrict(T& t, stream_offset off, stream_offset len = -1)
{ return restriction<T>(t, off, len); }

# endif // Borland 5.x, VC6-7.0 or GCC 2.9x //--------------------------------//
#endif // #ifndef BOOST_IOSTREAMS_BROKEN_OVERLOAD_RESOLUTION //---------------//
//----------------------------------------------------------------------------//

namespace detail {

//--------------Implementation of restricted_indirect_device------------------//

template<typename Device>
restricted_indirect_device<Device>::restricted_indirect_device
    (param_type dev, stream_offset off, stream_offset len)
    : basic_adapter<Device>(dev), beg_(off), pos_(off), 
      end_(len != -1 ? off + len : -1)
{
    if (len < -1 || off < 0)
        throw BOOST_IOSTREAMS_FAILURE("bad offset");
    iostreams::skip(this->component(), off);
}

template<typename Device>
inline std::streamsize restricted_indirect_device<Device>::read
    (char_type* s, std::streamsize n)
{
    using namespace std;
    streamsize amt =
        end_ != -1 ?
            (std::min) (n, static_cast<streamsize>(end_ - pos_)) :
            n;
    streamsize result = iostreams::read(this->component(), s, amt);
    if (result != -1)
        pos_ += result;
    return result;
}

template<typename Device>
inline std::streamsize restricted_indirect_device<Device>::write
    (const char_type* s, std::streamsize n)
{
    if (end_ != -1 && pos_ + n >= end_)
        bad_write();
    std::streamsize result = iostreams::write(this->component(), s, n);
    pos_ += result;
    return result;
}

template<typename Device>
std::streampos restricted_indirect_device<Device>::seek
    (stream_offset off, BOOST_IOS::seekdir way)
{
    stream_offset next;
    if (way == BOOST_IOS::beg) {
        next = beg_ + off;
    } else if (way == BOOST_IOS::cur) {
        next = pos_ + off;
    } else if (end_ != -1) {
        next = end_ + off;
    } else {
        // Restriction is half-open; seek relative to the actual end.
        pos_ = iostreams::seek(this->component(), off, BOOST_IOS::end);
        if (pos_ < beg_)
            bad_seek();
        return offset_to_position(pos_ - beg_);
    }
    if (next < beg_ || end_ != -1 && next >= end_)
        bad_seek();
    pos_ = iostreams::seek(this->component(), next - pos_, BOOST_IOS::cur);
    return offset_to_position(pos_ - beg_);
}

//--------------Implementation of restricted_direct_device--------------------//

template<typename Device>
restricted_direct_device<Device>::restricted_direct_device
    (const Device& dev, stream_offset off, stream_offset len)
    : basic_adapter<Device>(dev), beg_(0), end_(0)
{
    std::pair<char_type*, char_type*> seq =
        sequence(is_convertible<category, input>());
    if ( off < 0 || len < -1 || 
         len != -1 && off + len > seq.second - seq.first )
    {
        throw BOOST_IOSTREAMS_FAILURE("bad offset");
    }
    beg_ = seq.first + off;
    end_ = len != -1 ? 
        seq.first + off + len :
        seq.second;
}

template<typename Device>
typename restricted_direct_device<Device>::pair_type
restricted_direct_device<Device>::input_sequence()
{
    BOOST_STATIC_ASSERT((is_convertible<category, input>::value));
    return std::make_pair(beg_, end_);
}

template<typename Device>
typename restricted_direct_device<Device>::pair_type
restricted_direct_device<Device>::output_sequence()
{
    BOOST_STATIC_ASSERT((is_convertible<category, output>::value));
    return std::make_pair(beg_, end_);
}

template<typename Device>
typename restricted_direct_device<Device>::pair_type
restricted_direct_device<Device>::sequence(mpl::true_)
{ return iostreams::input_sequence(this->component()); }

template<typename Device>
typename restricted_direct_device<Device>::pair_type
restricted_direct_device<Device>::sequence(mpl::false_)
{ return iostreams::output_sequence(this->component()); }

//--------------Implementation of restricted_filter---------------------------//

template<typename Filter>
restricted_filter<Filter>::restricted_filter
    (const Filter& flt, stream_offset off, stream_offset len)
    : basic_adapter<Filter>(flt), beg_(off),
      pos_(off), end_(len != -1 ? off + len : -1), open_(false)
{
    if (len < -1 || off < 0)
        throw BOOST_IOSTREAMS_FAILURE("bad offset");
}

} // End namespace detail.

} } // End namespaces iostreams, boost.


#endif // #ifndef BOOST_IOSTREAMS_RESTRICT_HPP_INCLUDED
