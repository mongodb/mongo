/*
 *             Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

#include <syslog.h>

int main(int, char*[])
{
    ::openlog("test", LOG_NDELAY, LOG_USER);

    ::syslog(LOG_USER | LOG_DEBUG, "debug message");
    ::syslog(LOG_USER | LOG_INFO, "info message");
    ::syslog(LOG_USER | LOG_NOTICE, "notice message");
    ::syslog(LOG_USER | LOG_WARNING, "warning message");
    ::syslog(LOG_USER | LOG_ERR, "error message");
    ::syslog(LOG_USER | LOG_CRIT, "critical message");
    ::syslog(LOG_USER | LOG_ALERT, "alert message");
    ::syslog(LOG_USER | LOG_EMERG, "emergency message");

    ::closelog();

    return 0;
}
