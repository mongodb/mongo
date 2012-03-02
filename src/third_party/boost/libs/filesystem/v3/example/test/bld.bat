@echo off
rem Copyright Beman Dawes, 2010
rem Distributed under the Boost Software License, Version 1.0.
rem See www.boost.org/LICENSE_1_0.txt

bjam %* >bjam.log
find "error" <bjam.log
