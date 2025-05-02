// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <boost/asio/windows/basic_object_handle.hpp>
#endif


#if defined(BOOST_PROCESS_V2_WINDOWS)

#include <boost/process/v2/windows/default_launcher.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace windows
{

  std::size_t default_launcher::escaped_argv_length(basic_string_view<wchar_t> ws)
  {
    if (ws.empty())
      return 2u; // just quotes

    constexpr static auto space = L' ';
    constexpr static auto quote = L'"';

    const auto has_space = ws.find(space) != basic_string_view<wchar_t>::npos;
    const auto quoted = (ws.front() == quote) && (ws.back() == quote);
    const auto needs_escape = has_space && !quoted ;

    if (!needs_escape)
      return ws.size();
    else
      return ws.size() + std::count(ws.begin(), ws.end(), quote) + 2u;
  }


  std::size_t default_launcher::escape_argv_string(wchar_t * itr, std::size_t max_size, 
                                        basic_string_view<wchar_t> ws)
  { 
    const auto sz = escaped_argv_length(ws);
    if (sz > max_size)
      return 0u;
    if (ws.empty())      
    {
      itr[0] = L'"';
      itr[1] = L'"';
      return 2u;
    }

    const auto has_space = ws.find(L' ') != basic_string_view<wchar_t>::npos;
    const auto quoted = (ws.front() == L'"') && (ws.back() ==  L'"');
    const auto needs_escape = has_space && !quoted;

    if (!needs_escape)
      return std::copy(ws.begin(), ws.end(), itr) - itr;

    if (sz < (2u + ws.size()))
      return 0u;
      
    const auto end = itr + sz; 
    const auto begin = itr;
    *(itr ++) = L'"';
    for (auto wc : ws)
    {
      if (wc == L'"')
        *(itr++) = L'\\';
      *(itr++) = wc;
    }

    *(itr ++) = L'"';
    return itr - begin;
  }

  LPPROC_THREAD_ATTRIBUTE_LIST default_launcher::get_thread_attribute_list(error_code & ec)
  {
    if (startup_info.lpAttributeList != nullptr)
      return startup_info.lpAttributeList;
    SIZE_T size;
    if (!(::InitializeProcThreadAttributeList(NULL, 1, 0, &size) ||
          GetLastError() == ERROR_INSUFFICIENT_BUFFER))
    {
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
      return nullptr;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(::HeapAlloc(::GetProcessHeap(), 0, size));
    if (lpAttributeList == nullptr)
      return nullptr;

    if (!::InitializeProcThreadAttributeList(lpAttributeList, 1, 0, &size))
    {
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
      ::HeapFree(GetProcessHeap(), 0, lpAttributeList);
      return nullptr;
    }

    proc_attribute_list_storage.reset(lpAttributeList);
    startup_info.lpAttributeList = proc_attribute_list_storage.get();
    return startup_info.lpAttributeList;
  }

  void default_launcher::set_handle_list(error_code & ec)
  {
    auto tl = get_thread_attribute_list(ec);
    if (ec)
      return;
    if (!::UpdateProcThreadAttribute(
        tl, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited_handles.data(), inherited_handles.size() * sizeof(HANDLE), nullptr, nullptr))
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
  }

}
BOOST_PROCESS_V2_END_NAMESPACE

#endif