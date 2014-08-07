/* Copyright (c) 2002,2003 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE_1_0.txt or http://www.boost.org/LICENSE_1_0.txt)
 * Author: Jeff Garland 
 * $Date: 2008-02-27 12:00:24 -0800 (Wed, 27 Feb 2008) $
 */


/** @defgroup date_basics Date Basics
  This page summarizes some of the key user types and functions needed 
  to write programs using the gregorian date system.  This is not a
  comprehensive list, but rather some key types to start exploring.


**/ 

/** @defgroup date_alg Date Algorithms / Generators
  Date algorithms or generators are tools for generating other dates or
  schedules of dates.  A generator function starts with some part of a
  date such as a month and day and is supplied another part to then
  generate a final date.

**/ 

/** @defgroup date_format Date Formatting
  The functions on these page are some of the key formatting functions
  for dates.  
**/ 


//File doesn't have a current purpose except to generate docs
//and keep it changeable without recompiles
/*! @example days_alive.cpp 
  Calculate the number of days you have been living using durations and dates.
*/
/*! @example days_till_new_year.cpp 
  Calculate the number of days till new years
*/
/*! @example print_month.cpp 
  Simple utility to print out days of the month with the days of a month.  Demontstrates date iteration (date_time::date_itr). 
*/
/*! @example localization.cpp
  An example showing localized stream-based I/O.
*/
/*! @example dates_as_strings.cpp 
  Various parsing and output of strings (mostly supported for 
  compilers that do not support localized streams).
*/
/*! @example period_calc.cpp 
  Calculates if a date is in an 'irregular' collection of periods using
  period calculation functions.
*/
/*! @example print_holidays.cpp
  This is an example of using functors to define a holiday schedule
 */
/*! @example localization.cpp
  Demonstrates the use of facets to localize date output for Gregorian dates.
 */

 

