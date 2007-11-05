// (C) Copyright Jonathan Turkanis 2005.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

#ifndef BOOST_IOSTREAMS_TEE_HPP_INCLUDED
#define BOOST_IOSTREAMS_TEE_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <cassert>
#include <boost/config.hpp>  // BOOST_DEDUCE_TYPENAME.
#include <boost/iostreams/categories.hpp>
#include <boost/iostreams/detail/adapter/basic_adapter.hpp>
#include <boost/iostreams/detail/call_traits.hpp>
#include <boost/iostreams/detail/closer.hpp>
#include <boost/iostreams/operations.hpp>
#include <boost/iostreams/pipeline.hpp>
#include <boost/iostreams/traits.hpp>
#include <boost/static_assert.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/is_same.hpp>

namespace boost { namespace iostreams {

//
// Template name: tee_filter.
// Template paramters:
//      Device - A blocking Sink.
//
template<typename Device>
class tee_filter : public detail::basic_adapter<Device> {
public:
    typedef typename detail::param_type<Device>::type  param_type;
    typedef typename char_type_of<Device>::type        char_type;
    struct category
        : multichar_output_filter_tag,
          closable_tag,
          flushable_tag,
          localizable_tag,
          optimally_buffered_tag
        { };

    BOOST_STATIC_ASSERT((
        is_convertible< // Using mode_of causes failures on VC6-7.0.
            BOOST_DEDUCED_TYPENAME iostreams::category_of<Device>::type, output
        >::value
    ));

    explicit tee_filter(param_type dev) 
        : detail::basic_adapter<Device>(dev) 
        { }

    template<typename Sink>
    std::streamsize write(Sink& snk, const char_type* s, std::streamsize n)
    {
        std::streamsize result = iostreams::write(snk, s, n);
        std::streamsize result2 = iostreams::write(this->component(), s, result);
        (void) result2; // Suppress 'unused variable' warning.
        assert(result == result2);
        return result;
    }

    template<typename Next>
    void close( Next&,
                BOOST_IOS::openmode which =
                    BOOST_IOS::in | BOOST_IOS::out )
    { iostreams::close(this->component(), which); }

    template<typename Sink>
    bool flush(Sink& snk)
    {
        bool r1 = iostreams::flush(snk);
        bool r2 = iostreams::flush(this->component());
        return r1 && r2;
    }
};
BOOST_IOSTREAMS_PIPABLE(tee_filter, 1)

//
// Template name: tee_device.
// Template paramters:
//      Sink1 - A blocking Sink.
//      Sink2 - A blocking Sink.
//
template<typename Sink1, typename Sink2>
class tee_device {
public:
    typedef typename detail::param_type<Sink1>::type  param_type1;
    typedef typename detail::param_type<Sink2>::type  param_type2;
    typedef typename detail::value_type<Sink1>::type  value_type1;
    typedef typename detail::value_type<Sink2>::type  value_type2;
    typedef typename char_type_of<Sink1>::type        char_type;
    BOOST_STATIC_ASSERT((
        is_same<
            char_type, 
            BOOST_DEDUCED_TYPENAME char_type_of<Sink2>::type
        >::value
    ));
    BOOST_STATIC_ASSERT((
        is_convertible< // Using mode_of causes failures on VC6-7.0.
            BOOST_DEDUCED_TYPENAME iostreams::category_of<Sink1>::type, output
        >::value
    ));
    BOOST_STATIC_ASSERT((
        is_convertible< // Using mode_of causes failures on VC6-7.0.
            BOOST_DEDUCED_TYPENAME iostreams::category_of<Sink2>::type, output
        >::value
    ));
    struct category
        : output,
          device_tag,
          closable_tag,
          flushable_tag,
          localizable_tag,
          optimally_buffered_tag
        { };
    tee_device(param_type1 sink1, param_type2 sink2) 
        : sink1_(sink1), sink2_(sink2)
        { }
    std::streamsize write(const char_type* s, std::streamsize n)
    {
        std::streamsize result1 = iostreams::write(sink1_, s, n);
        std::streamsize result2 = iostreams::write(sink2_, s, n);
        (void) result1; // Suppress 'unused variable' warning.
        (void) result2;
        assert(result1 == n && result2 == n);
        return n;
    }
    void close(BOOST_IOS::openmode which = BOOST_IOS::in | BOOST_IOS::out)
    { 
        detail::external_closer<Sink2> close2(sink2_, which);
        detail::external_closer<Sink1> close1(sink1_, which);
    }
    bool flush()
    {
        bool r1 = iostreams::flush(sink1_);
        bool r2 = iostreams::flush(sink2_);
        return r1 && r2;
    }
    template<typename Locale>
    void imbue(const Locale& loc)
    {
        iostreams::imbue(sink1_, loc);
        iostreams::imbue(sink2_, loc);
    }
    std::streamsize optimal_buffer_size() const 
    {
        return (std::max) ( iostreams::optimal_buffer_size(sink1_), 
                            iostreams::optimal_buffer_size(sink2_) );
    }
private:
    value_type1 sink1_;
    value_type2 sink2_;
};

template<typename Sink>
tee_filter<Sink> tee(const Sink& snk) 
{ return tee_filter<Sink>(snk); }

template<typename Sink1, typename Sink2>
tee_device<Sink1, Sink2> tee(const Sink1& sink1, const Sink2& sink2) 
{ return tee_device<Sink1, Sink2>(sink1, sink2); }

} } // End namespaces iostreams, boost.

#endif // #ifndef BOOST_IOSTREAMS_TEE_HPP_INCLUDED
