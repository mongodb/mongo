;/*
; *          Copyright Andrey Semashev 2007 - 2015.
; * Distributed under the Boost Software License, Version 1.0.
; *    (See accompanying file LICENSE_1_0.txt or copy at
; *          http://www.boost.org/LICENSE_1_0.txt)
; *
; * This file is the Boost.Log library implementation, see the library documentation
; * at http://www.boost.org/libs/log/doc/log.html.
; */
;
;/* --------------------------------------------------------
; * HEADER SECTION
; */
SeverityNames=(Debug=0x0:BOOST_LOG_SEVERITY_DEBUG
               Info=0x1:BOOST_LOG_SEVERITY_INFO
               Warning=0x2:BOOST_LOG_SEVERITY_WARNING
               Error=0x3:BOOST_LOG_SEVERITY_ERROR
              )
;
;
;
;/* ------------------------------------------------------------------
; * MESSAGE DEFINITION SECTION
; */

MessageIdTypedef=DWORD

MessageId=0x100
Severity=Debug
Facility=Application
SymbolicName=BOOST_LOG_MSG_DEBUG
Language=English
%1
.

MessageId=0x101
Severity=Info
Facility=Application
SymbolicName=BOOST_LOG_MSG_INFO
Language=English
%1
.

MessageId=0x102
Severity=Warning
Facility=Application
SymbolicName=BOOST_LOG_MSG_WARNING
Language=English
%1
.

MessageId=0x103
Severity=Error
Facility=Application
SymbolicName=BOOST_LOG_MSG_ERROR
Language=English
%1
.
