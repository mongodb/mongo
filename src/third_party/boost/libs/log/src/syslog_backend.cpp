/*
 *          Copyright Andrey Semashev 2007 - 2020.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   syslog_backend.cpp
 * \author Andrey Semashev
 * \date   08.01.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>

#ifndef BOOST_LOG_WITHOUT_SYSLOG

#include <ctime>
#include <algorithm>
#include <stdexcept>
#include <boost/limits.hpp>
#include <boost/assert.hpp>
#include <boost/smart_ptr/weak_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/throw_exception.hpp>
#if !defined(BOOST_LOG_NO_ASIO)
#include <boost/asio/buffer.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/resolver_base.hpp>
#endif
#include <boost/system/error_code.hpp>
#include <boost/date_time/c_time.hpp>
#include <boost/log/sinks/syslog_backend.hpp>
#include <boost/log/sinks/syslog_constants.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/detail/snprintf.hpp>
#include <boost/log/exceptions.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#endif
#include "unique_ptr.hpp"

#ifdef BOOST_LOG_USE_NATIVE_SYSLOG
#include <syslog.h>
#endif // BOOST_LOG_USE_NATIVE_SYSLOG

#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sinks {

namespace syslog {

    //! The function constructs log record level from an integer
    BOOST_LOG_API level make_level(int lev)
    {
        if (BOOST_UNLIKELY(static_cast< unsigned int >(lev) >= 8u))
            BOOST_THROW_EXCEPTION(std::out_of_range("syslog level value is out of range"));
        return static_cast< level >(lev);
    }

    //! The function constructs log source facility from an integer
    BOOST_LOG_API facility make_facility(int fac)
    {
        if (BOOST_UNLIKELY((static_cast< unsigned int >(fac) & 7u) != 0u
            || static_cast< unsigned int >(fac) > (23u * 8u)))
        {
            BOOST_THROW_EXCEPTION(std::out_of_range("syslog facility code value is out of range"));
        }
        return static_cast< facility >(fac);
    }

} // namespace syslog

////////////////////////////////////////////////////////////////////////////////
//! Syslog sink backend implementation
////////////////////////////////////////////////////////////////////////////////
struct syslog_backend::implementation
{
#ifdef BOOST_LOG_USE_NATIVE_SYSLOG
    struct native;
#endif // BOOST_LOG_USE_NATIVE_SYSLOG
#if !defined(BOOST_LOG_NO_ASIO)
    struct udp_socket_based;
#endif

    //! Level mapper
    severity_mapper_type m_LevelMapper;

    //! Logging facility (portable or native, depending on the backend implementation)
    const int m_Facility;

    //! Constructor
    explicit implementation(int facility) :
        m_Facility(facility)
    {
    }
    //! Virtual destructor
    virtual ~implementation() {}

    //! The method sends the formatted message to the syslog host
    virtual void send(syslog::level lev, string_type const& formatted_message) = 0;
};


////////////////////////////////////////////////////////////////////////////////
//  Native syslog API support
////////////////////////////////////////////////////////////////////////////////

#ifdef BOOST_LOG_USE_NATIVE_SYSLOG

BOOST_LOG_ANONYMOUS_NAMESPACE {

    //! Syslog service initializer (implemented as a weak singleton)
#if !defined(BOOST_LOG_NO_THREADS)
    class native_syslog_initializer :
        private log::aux::lazy_singleton< native_syslog_initializer, mutex >
#else
    class native_syslog_initializer
#endif
    {
#if !defined(BOOST_LOG_NO_THREADS)
        friend class log::aux::lazy_singleton< native_syslog_initializer, mutex >;
        typedef log::aux::lazy_singleton< native_syslog_initializer, mutex > mutex_holder;
#endif

    private:
        /*!
         * \brief Application identification string
         *
         * \note We have to keep it as an immutable member because some syslog implementations (e.g. glibc)
         *       do not deep-copy the ident string to internal storage when \c openlog is called
         *       and instead save a pointer to the user-provided string. This means the user-provided
         *       string needs to remain accessible for the whole duration of logging.
         *
         *       https://github.com/boostorg/log/issues/97
         *       https://sourceware.org/bugzilla/show_bug.cgi?id=25442
         */
        const std::string m_Ident;

    public:
        native_syslog_initializer(std::string const& ident, int facility) :
            m_Ident(ident)
        {
            ::openlog((m_Ident.empty() ? static_cast< const char* >(NULL) : m_Ident.c_str()), 0, facility);
        }
        ~native_syslog_initializer()
        {
            ::closelog();
        }

        static shared_ptr< native_syslog_initializer > get_instance(std::string const& ident, int facility)
        {
#if !defined(BOOST_LOG_NO_THREADS)
            lock_guard< mutex > lock(mutex_holder::get());
#endif
            static weak_ptr< native_syslog_initializer > instance;
            shared_ptr< native_syslog_initializer > p(instance.lock());
            if (!p)
            {
                p = boost::make_shared< native_syslog_initializer >(ident, facility);
                instance = p;
            }
            return p;
        }

        BOOST_DELETED_FUNCTION(native_syslog_initializer(native_syslog_initializer const&))
        BOOST_DELETED_FUNCTION(native_syslog_initializer& operator= (native_syslog_initializer const&))
    };

} // namespace

struct syslog_backend::implementation::native :
    public implementation
{
    //! Reference to the syslog service initializer
    const shared_ptr< native_syslog_initializer > m_pSyslogInitializer;

    //! Constructor
    native(syslog::facility const& fac, std::string const& ident) :
        implementation(convert_facility(fac)),
        m_pSyslogInitializer(native_syslog_initializer::get_instance(ident, this->m_Facility))
    {
    }

    //! The method sends the formatted message to the syslog host
    void send(syslog::level lev, string_type const& formatted_message) BOOST_OVERRIDE
    {
        int native_level;
        switch (lev)
        {
        case syslog::emergency:
            native_level = LOG_EMERG; break;
        case syslog::alert:
            native_level = LOG_ALERT; break;
        case syslog::critical:
            native_level = LOG_CRIT; break;
        case syslog::error:
            native_level = LOG_ERR; break;
        case syslog::warning:
            native_level = LOG_WARNING; break;
        case syslog::notice:
            native_level = LOG_NOTICE; break;
        case syslog::debug:
            native_level = LOG_DEBUG; break;
        default:
            native_level = LOG_INFO; break;
        }

        ::syslog(this->m_Facility | native_level, "%s", formatted_message.c_str());
    }

private:
    //! The function converts portable facility codes to the native codes
    static int convert_facility(syslog::facility const& fac)
    {
        // POSIX does not specify anything except for LOG_USER and LOG_LOCAL*
        #ifndef LOG_KERN
        #define LOG_KERN LOG_USER
        #endif
        #ifndef LOG_DAEMON
        #define LOG_DAEMON LOG_KERN
        #endif
        #ifndef LOG_MAIL
        #define LOG_MAIL LOG_USER
        #endif
        #ifndef LOG_AUTH
        #define LOG_AUTH LOG_DAEMON
        #endif
        #ifndef LOG_SYSLOG
        #define LOG_SYSLOG LOG_DAEMON
        #endif
        #ifndef LOG_LPR
        #define LOG_LPR LOG_DAEMON
        #endif
        #ifndef LOG_NEWS
        #define LOG_NEWS LOG_USER
        #endif
        #ifndef LOG_UUCP
        #define LOG_UUCP LOG_USER
        #endif
        #ifndef LOG_CRON
        #define LOG_CRON LOG_DAEMON
        #endif
        #ifndef LOG_AUTHPRIV
        #define LOG_AUTHPRIV LOG_AUTH
        #endif
        #ifndef LOG_FTP
        #define LOG_FTP LOG_DAEMON
        #endif

        static const int native_facilities[24] =
        {
            LOG_KERN,
            LOG_USER,
            LOG_MAIL,
            LOG_DAEMON,
            LOG_AUTH,
            LOG_SYSLOG,
            LOG_LPR,
            LOG_NEWS,
            LOG_UUCP,
            LOG_CRON,
            LOG_AUTHPRIV,
            LOG_FTP,

            // reserved values
            LOG_USER,
            LOG_USER,
            LOG_USER,
            LOG_USER,

            LOG_LOCAL0,
            LOG_LOCAL1,
            LOG_LOCAL2,
            LOG_LOCAL3,
            LOG_LOCAL4,
            LOG_LOCAL5,
            LOG_LOCAL6,
            LOG_LOCAL7
        };

        std::size_t n = static_cast< unsigned int >(fac) / 8u;
        BOOST_ASSERT(n < sizeof(native_facilities) / sizeof(*native_facilities));
        return native_facilities[n];
    }
};

#endif // BOOST_LOG_USE_NATIVE_SYSLOG


////////////////////////////////////////////////////////////////////////////////
//  Socket-based implementation
////////////////////////////////////////////////////////////////////////////////

#if !defined(BOOST_LOG_NO_ASIO)

BOOST_LOG_ANONYMOUS_NAMESPACE {

    //! The shared UDP socket
    struct syslog_udp_socket
    {
    private:
        //! The socket primitive
        asio::ip::udp::socket m_Socket;

    public:
        //! The constructor creates a socket bound to the specified local address and port
        explicit syslog_udp_socket(asio::io_context& io_ctx, asio::ip::udp const& protocol, asio::ip::udp::endpoint const& local_address) :
            m_Socket(io_ctx)
        {
            m_Socket.open(protocol);
            m_Socket.set_option(asio::socket_base::reuse_address(true));
            m_Socket.bind(local_address);
        }
        //! The destructor closes the socket
        ~syslog_udp_socket()
        {
            boost::system::error_code ec;
            m_Socket.shutdown(asio::socket_base::shutdown_both, ec);
            m_Socket.close(ec);
        }

        //! The method sends the syslog message to the specified endpoint
        void send_message(int pri, const char* local_host_name, asio::ip::udp::endpoint const& target, const char* message);

        BOOST_DELETED_FUNCTION(syslog_udp_socket(syslog_udp_socket const&))
        BOOST_DELETED_FUNCTION(syslog_udp_socket& operator= (syslog_udp_socket const&))
    };

    //! The class contains the UDP service for syslog sockets to function
    class syslog_udp_service :
        public log::aux::lazy_singleton< syslog_udp_service, shared_ptr< syslog_udp_service > >
    {
        friend class log::aux::lazy_singleton< syslog_udp_service, shared_ptr< syslog_udp_service > >;
        typedef log::aux::lazy_singleton< syslog_udp_service, shared_ptr< syslog_udp_service > > base_type;

    public:
        //! The IO context instance
        asio::io_context m_IOContext;
        //! The local host name to put into log message
        std::string m_LocalHostName;

#if !defined(BOOST_LOG_NO_THREADS)
        //! A synchronization primitive to protect the host name resolver
        mutex m_Mutex;
        //! The resolver is used to acquire connection endpoints
        asio::ip::udp::resolver m_HostNameResolver;
#endif // !defined(BOOST_LOG_NO_THREADS)

    private:
        //! Default constructor
        syslog_udp_service()
#if !defined(BOOST_LOG_NO_THREADS)
            : m_HostNameResolver(m_IOContext)
#endif // !defined(BOOST_LOG_NO_THREADS)
        {
            boost::system::error_code err;
            m_LocalHostName = asio::ip::host_name(err);
        }
        //! Initializes the singleton instance
        static void init_instance()
        {
            base_type::get_instance().reset(new syslog_udp_service());
        }
    };

    //! The method sends the syslog message to the specified endpoint
    void syslog_udp_socket::send_message(
        int pri, const char* local_host_name, asio::ip::udp::endpoint const& target, const char* message)
    {
        std::time_t t = std::time(NULL);
        std::tm ts;
        std::tm* time_stamp = boost::date_time::c_time::localtime(&t, &ts);

        // Month will have to be injected separately, as involving locale won't do here
        static const char months[12][4] =
        {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };

        // The packet size is mandated in RFC3164, plus one for the terminating zero
        char packet[1025];
        int n = boost::log::aux::snprintf
        (
            packet,
            sizeof(packet),
            "<%d>%s %2d %02d:%02d:%02d %s %s",
            pri,
            months[time_stamp->tm_mon],
            time_stamp->tm_mday,
            time_stamp->tm_hour,
            time_stamp->tm_min,
            time_stamp->tm_sec,
            local_host_name,
            message
        );
        if (BOOST_LIKELY(n > 0))
        {
            std::size_t packet_size = static_cast< std::size_t >(n) >= sizeof(packet) ? sizeof(packet) - 1u : static_cast< std::size_t >(n);
            m_Socket.send_to(asio::buffer(packet, packet_size), target);
        }
    }

} // namespace

struct syslog_backend::implementation::udp_socket_based :
    public implementation
{
    //! Protocol to be used
    asio::ip::udp m_Protocol;
    //! Pointer to the list of sockets
    shared_ptr< syslog_udp_service > m_pService;
    //! Pointer to the socket being used
    log::aux::unique_ptr< syslog_udp_socket > m_pSocket;
    //! The target host to send packets to
    asio::ip::udp::endpoint m_TargetHost;

    //! Constructor
    explicit udp_socket_based(syslog::facility const& fac, asio::ip::udp const& protocol) :
        implementation(fac),
        m_Protocol(protocol),
        m_pService(syslog_udp_service::get())
    {
        if (m_Protocol == asio::ip::udp::v4())
        {
            m_TargetHost = asio::ip::udp::endpoint(asio::ip::address_v4(0x7F000001), 514); // 127.0.0.1:514
        }
        else
        {
            // ::1, port 514
            asio::ip::address_v6::bytes_type addr;
            std::fill_n(addr.data(), addr.size() - 1u, static_cast< unsigned char >(0u));
            addr[addr.size() - 1u] = 1u;
            m_TargetHost = asio::ip::udp::endpoint(asio::ip::address_v6(addr), 514);
        }
    }

    //! The method sends the formatted message to the syslog host
    void send(syslog::level lev, string_type const& formatted_message) BOOST_OVERRIDE
    {
        if (!m_pSocket.get())
        {
            asio::ip::udp::endpoint any_local_address;
            m_pSocket.reset(new syslog_udp_socket(m_pService->m_IOContext, m_Protocol, any_local_address));
        }

        m_pSocket->send_message(
            this->m_Facility | static_cast< int >(lev),
            m_pService->m_LocalHostName.c_str(),
            m_TargetHost,
            formatted_message.c_str());
    }
};

#endif // !defined(BOOST_LOG_NO_ASIO)

////////////////////////////////////////////////////////////////////////////////
//  Sink backend implementation
////////////////////////////////////////////////////////////////////////////////
BOOST_LOG_API syslog_backend::syslog_backend()
{
    construct(log::aux::empty_arg_list());
}

//! Destructor
BOOST_LOG_API syslog_backend::~syslog_backend()
{
    delete m_pImpl;
}

//! The method installs the function object that maps application severity levels to Syslog levels
BOOST_LOG_API void syslog_backend::set_severity_mapper(severity_mapper_type const& mapper)
{
    m_pImpl->m_LevelMapper = mapper;
}

//! The method writes the message to the sink
BOOST_LOG_API void syslog_backend::consume(record_view const& rec, string_type const& formatted_message)
{
    m_pImpl->send(
        m_pImpl->m_LevelMapper.empty() ? syslog::info : m_pImpl->m_LevelMapper(rec),
        formatted_message);
}


//! The method creates the backend implementation
BOOST_LOG_API void syslog_backend::construct(syslog::facility fac, syslog::impl_types use_impl, ip_versions ip_version, std::string const& ident)
{
#ifdef BOOST_LOG_USE_NATIVE_SYSLOG
    if (use_impl == syslog::native)
    {
        typedef implementation::native native_impl;
        m_pImpl = new native_impl(fac, ident);
        return;
    }
#endif // BOOST_LOG_USE_NATIVE_SYSLOG

#if !defined(BOOST_LOG_NO_ASIO)
    typedef implementation::udp_socket_based udp_socket_based_impl;
    asio::ip::udp protocol = asio::ip::udp::v4();
    switch (ip_version)
    {
    case v4:
        break;
    case v6:
        protocol = asio::ip::udp::v6();
        break;
    default:
        BOOST_LOG_THROW_DESCR(setup_error, "Incorrect IP version specified");
    }

    m_pImpl = new udp_socket_based_impl(fac, protocol);
#endif
}

#if !defined(BOOST_LOG_NO_ASIO)

//! The method sets the local address which log records will be sent from.
BOOST_LOG_API void syslog_backend::set_local_address(std::string const& addr, unsigned short port)
{
#if !defined(BOOST_LOG_NO_THREADS)
    typedef implementation::udp_socket_based udp_socket_based_impl;
    if (udp_socket_based_impl* impl = dynamic_cast< udp_socket_based_impl* >(m_pImpl))
    {
        char service_name[std::numeric_limits< unsigned int >::digits10 + 3];
        boost::log::aux::snprintf(service_name, sizeof(service_name), "%u", static_cast< unsigned int >(port));

        asio::ip::udp::endpoint local_address;
        {
            lock_guard< mutex > lock(impl->m_pService->m_Mutex);
            asio::ip::udp::resolver::results_type results = impl->m_pService->m_HostNameResolver.resolve
            (
                impl->m_Protocol,
                addr,
                service_name,
                asio::ip::resolver_base::address_configured | asio::ip::resolver_base::passive
            );

            local_address = *results.cbegin();
        }

        impl->m_pSocket.reset(new syslog_udp_socket(impl->m_pService->m_IOContext, impl->m_Protocol, local_address));
    }
#else
    // Boost.ASIO requires threads for the host name resolver,
    // so without threads we simply assume the string already contains IP address
    set_local_address(boost::asio::ip::address::from_string(addr), port);
#endif // !defined(BOOST_LOG_NO_THREADS)
}
//! The method sets the local address which log records will be sent from.
BOOST_LOG_API void syslog_backend::set_local_address(boost::asio::ip::address const& addr, unsigned short port)
{
    typedef implementation::udp_socket_based udp_socket_based_impl;
    if (udp_socket_based_impl* impl = dynamic_cast< udp_socket_based_impl* >(m_pImpl))
    {
        if ((impl->m_Protocol == asio::ip::udp::v4() && !addr.is_v4()) || (impl->m_Protocol == asio::ip::udp::v6() && !addr.is_v6()))
            BOOST_LOG_THROW_DESCR(setup_error, "Incorrect IP version specified in the local address");

        impl->m_pSocket.reset(new syslog_udp_socket(
            impl->m_pService->m_IOContext, impl->m_Protocol, asio::ip::udp::endpoint(addr, port)));
    }
}

//! The method sets the address of the remote host where log records will be sent to.
BOOST_LOG_API void syslog_backend::set_target_address(std::string const& addr, unsigned short port)
{
#if !defined(BOOST_LOG_NO_THREADS)
    typedef implementation::udp_socket_based udp_socket_based_impl;
    if (udp_socket_based_impl* impl = dynamic_cast< udp_socket_based_impl* >(m_pImpl))
    {
        char service_name[std::numeric_limits< unsigned int >::digits10 + 3];
        boost::log::aux::snprintf(service_name, sizeof(service_name), "%u", static_cast< unsigned int >(port));

        asio::ip::udp::endpoint remote_address;
        {
            lock_guard< mutex > lock(impl->m_pService->m_Mutex);
            asio::ip::udp::resolver::results_type results = impl->m_pService->m_HostNameResolver.resolve
            (
                impl->m_Protocol,
                addr,
                service_name,
                asio::ip::resolver_query_base::address_configured
            );

            remote_address = *results.cbegin();
        }

        impl->m_TargetHost = remote_address;
    }
#else
    // Boost.ASIO requires threads for the host name resolver,
    // so without threads we simply assume the string already contains IP address
    set_target_address(boost::asio::ip::address::from_string(addr), port);
#endif // !defined(BOOST_LOG_NO_THREADS)
}
//! The method sets the address of the remote host where log records will be sent to.
BOOST_LOG_API void syslog_backend::set_target_address(boost::asio::ip::address const& addr, unsigned short port)
{
    typedef implementation::udp_socket_based udp_socket_based_impl;
    if (udp_socket_based_impl* impl = dynamic_cast< udp_socket_based_impl* >(m_pImpl))
    {
        if ((impl->m_Protocol == asio::ip::udp::v4() && !addr.is_v4()) || (impl->m_Protocol == asio::ip::udp::v6() && !addr.is_v6()))
            BOOST_LOG_THROW_DESCR(setup_error, "Incorrect IP version specified in the target address");

        impl->m_TargetHost = asio::ip::udp::endpoint(addr, port);
    }
}

#endif // !defined(BOOST_LOG_NO_ASIO)

} // namespace sinks

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // !defined(BOOST_LOG_WITHOUT_SYSLOG)
