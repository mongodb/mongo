/*
 *
 * Copyright (c) 1998-2004
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */

 /*
  *   LOCATION:    see http://www.boost.org for most recent version.
  *   FILE:        regex.cpp
  *   VERSION:     see <boost/version.hpp>
  *   DESCRIPTION: Misc boost::regbase member funnctions.
  */


#define BOOST_REGEX_SOURCE

#include <boost/regex/config.hpp>

#ifdef BOOST_REGEX_HAS_MS_STACK_GUARD

#include <malloc.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define NOGDI
#define NOUSER
#include <windows.h>
#include <stdexcept>
#include <boost/regex/pattern_except.hpp>
#include <boost/regex/v4/protected_call.hpp>

namespace boost {
namespace BOOST_REGEX_DETAIL_NS {

static void execute_eror()
{
   // we only get here after a stack overflow,
   // this has to be a separate proceedure because we 
   // can't mix __try{}__except block with local objects  
   // that have destructors:
   reset_stack_guard_page();
   std::runtime_error err("Out of stack space, while attempting to match a regular expression.");
   raise_runtime_error(err);
}

bool BOOST_REGEX_CALL abstract_protected_call::execute()const
{
   __try{
      return this->call();
   }__except(EXCEPTION_STACK_OVERFLOW == GetExceptionCode())
   {
      execute_eror();
   }
   // We never really get here at all:
   return false;
}

BOOST_REGEX_DECL void BOOST_REGEX_CALL reset_stack_guard_page()
{
#if defined(BOOST_REGEX_HAS_MS_STACK_GUARD) && defined(_MSC_VER) && (_MSC_VER >= 1300)
   _resetstkoflw();
#else
   //
   // We need to locate the current page being used by the stack,
   // move to the page below it and then deallocate and protect
   // that page.  Note that ideally we would protect only the lowest
   // stack page that has been allocated: in practice there
   // seems to be no easy way to locate this page, in any case as
   // long as the next page is protected, then Windows will figure
   // the rest out for us...
   //
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   MEMORY_BASIC_INFORMATION mi;
   DWORD previous_protection_status;
   //
   // this is an address in our stack space:
   //
   LPBYTE page = (LPBYTE)&page;
   //
   // Get the current memory page in use:
   //
   VirtualQuery(page, &mi, sizeof(mi));
   //
   // Go to the page one below this:
   //
   page = (LPBYTE)(mi.BaseAddress)-si.dwPageSize;
   //
   // Free and protect everything from the start of the
   // allocation range, to the end of the page below the
   // one in use:
   //
   if (!VirtualFree(mi.AllocationBase, (LPBYTE)page - (LPBYTE)mi.AllocationBase, MEM_DECOMMIT)
      || !VirtualProtect(page, si.dwPageSize, PAGE_GUARD | PAGE_READWRITE, &previous_protection_status))
   {
      throw std::bad_exception();
   }
#endif
}
}
} // namspaces
#endif

#if defined(BOOST_RE_USE_VCL) && defined(BOOST_REGEX_DYN_LINK)

int WINAPI DllEntryPoint(HINSTANCE , unsigned long , void*)
{
   return 1;
}
#endif

