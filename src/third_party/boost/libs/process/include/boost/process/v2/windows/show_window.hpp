//
// boost/process/v2/windows/default_launcher.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_WINDOWS_SHOW_WINDOW_HPP
#define BOOST_PROCESS_V2_WINDOWS_SHOW_WINDOW_HPP

#include <boost/process/v2/windows/default_launcher.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace windows
{

/// A templated initializer to add wShowWindow flags.
template<DWORD Flags>
struct process_show_window
{
  constexpr process_show_window() {}

  error_code on_setup(windows::default_launcher & launcher, 
                      const filesystem::path &, 
                      const std::wstring &) const
  {
    launcher.startup_info.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
    launcher.startup_info.StartupInfo.wShowWindow |= Flags;

    return error_code {};
  };
};

///Hides the window and activates another window.
constexpr static process_show_window<SW_HIDE           > show_window_hide;
///Activates the window and displays it as a maximized window.
constexpr static process_show_window<SW_SHOWMAXIMIZED  > show_window_maximized;
///Activates the window and displays it as a minimized window.
constexpr static process_show_window<SW_SHOWMINIMIZED  > show_window_minimized;
///Displays the window as a minimized window. This value is similar to `minimized`, except the window is not activated.
constexpr static process_show_window<SW_SHOWMINNOACTIVE> show_window_minimized_not_active;
///Displays a window in its most recent size and position. This value is similar to show_normal`, except that the window is not activated.
constexpr static process_show_window<SW_SHOWNOACTIVATE > show_window_not_active;
///Activates and displays a window. If the window is minimized or maximized, the system restores it to its original size and position. An application should specify this flag when displaying the window for the first time.
constexpr static process_show_window<SW_SHOWNORMAL     > show_window_normal;

}
BOOST_PROCESS_V2_END_NAMESPACE

#endif //  BOOST_PROCESS_V2_WINDOWS_SHOW_WINDOW_HPP