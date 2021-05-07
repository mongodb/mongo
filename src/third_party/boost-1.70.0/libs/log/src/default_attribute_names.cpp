/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   default_attribute_names.cpp
 * \author Andrey Semashev
 * \date   13.07.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/log/detail/default_attribute_names.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

namespace default_attribute_names {

BOOST_LOG_ANONYMOUS_NAMESPACE {

    class names :
        public lazy_singleton< names, shared_ptr< names > >
    {
    private:
        typedef lazy_singleton< names, shared_ptr< names > > base_type;

#if !defined(BOOST_LOG_BROKEN_FRIEND_TEMPLATE_SPECIALIZATIONS)
        friend class lazy_singleton< names, shared_ptr< names > >;
#else
        friend class base_type;
#endif

    public:
        const attribute_name severity;
        const attribute_name channel;
        const attribute_name message;
        const attribute_name line_id;
        const attribute_name timestamp;
        const attribute_name process_id;
        const attribute_name thread_id;

    private:
        names() :
            severity("Severity"),
            channel("Channel"),
            message("Message"),
            line_id("LineID"),
            timestamp("TimeStamp"),
            process_id("ProcessID"),
            thread_id("ThreadID")
        {
        }

        static void init_instance()
        {
            get_instance().reset(new names());
        }

    public:
        static names& get()
        {
            return *base_type::get();
        }
    };

} // namespace

BOOST_LOG_API attribute_name severity()
{
    return names::get().severity;
}

BOOST_LOG_API attribute_name channel()
{
    return names::get().channel;
}

BOOST_LOG_API attribute_name message()
{
    return names::get().message;
}

BOOST_LOG_API attribute_name line_id()
{
    return names::get().line_id;
}

BOOST_LOG_API attribute_name timestamp()
{
    return names::get().timestamp;
}

BOOST_LOG_API attribute_name process_id()
{
    return names::get().process_id;
}

BOOST_LOG_API attribute_name thread_id()
{
    return names::get().thread_id;
}

} // namespace default_attribute_names

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
