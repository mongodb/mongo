# - Find the Windows SDK aka Platform SDK
# taken from https://github.com/ampl/mp/blob/master/support/cmake/FindWindowsSDK.cmake
#
# Relevant Wikipedia article: http://en.wikipedia.org/wiki/Microsoft_Windows_SDK
#
# Pass "COMPONENTS tools" to ignore Visual Studio version checks: in case
# you just want the tool binaries to run, rather than the libraries and headers
# for compiling.
#
# Variables:
#  WINDOWSSDK_FOUND - if any version of the windows or platform SDK was found that is usable with the current version of visual studio
#  WINDOWSSDK_LATEST_DIR
#  WINDOWSSDK_LATEST_NAME
#  WINDOWSSDK_FOUND_PREFERENCE - if we found an entry indicating a "preferred" SDK listed for this visual studio version
#  WINDOWSSDK_PREFERRED_DIR
#  WINDOWSSDK_PREFERRED_NAME
#
#  WINDOWSSDK_DIRS - contains no duplicates, ordered most recent first.
#  WINDOWSSDK_PREFERRED_FIRST_DIRS - contains no duplicates, ordered with preferred first, followed by the rest in descending recency
#
# Functions:
#  GetUMWindowsSDKLibraryDir(<output variable>) - Find the latest SDK user mode (um) library directory,
#   architecture dependent
#  GetUMWindowsSDKIncludeDir(<output variable>) - Find the latest SDK user mode (um) include directory
#
#  windowssdk_name_lookup(<directory> <output variable>) - Find the name corresponding with the SDK directory you pass in, or
#     NOTFOUND if not recognized. Your directory must be one of WINDOWSSDK_DIRS for this to work.
#
#  windowssdk_build_lookup(<directory> <output variable>) - Find the build version number corresponding with the SDK directory you pass in, or
#     NOTFOUND if not recognized. Your directory must be one of WINDOWSSDK_DIRS for this to work.
#
#  get_windowssdk_from_component(<file or dir> <output variable>) - Given a library or include dir,
#     find the Windows SDK root dir corresponding to it, or NOTFOUND if unrecognized.
#
#  get_windowssdk_library_dirs(<directory> <output variable>) - Find the architecture-appropriate
#     library directories corresponding to the SDK directory you pass in (or NOTFOUND if none)
#
#  get_windowssdk_library_dirs_multiple(<output variable> <directory> ...) - Find the architecture-appropriate
#     library directories corresponding to the SDK directories you pass in, in order, skipping those not found. NOTFOUND if none at all.
#     Good for passing WINDOWSSDK_DIRS or WINDOWSSDK_DIRS to if you really just want a file and don't care where from.
#
#  get_windowssdk_include_dirs(<directory> <output variable>) - Find the
#     include directories corresponding to the SDK directory you pass in (or NOTFOUND if none)
#
#  get_windowssdk_include_dirs_multiple(<output variable> <directory> ...) - Find the
#     include directories corresponding to the SDK directories you pass in, in order, skipping those not found. NOTFOUND if none at all.
#     Good for passing WINDOWSSDK_DIRS or WINDOWSSDK_DIRS to if you really just want a file and don't care where from.
#
# Requires these CMake modules:
#  FindPackageHandleStandardArgs (known included with CMake >=2.6.2)
#
# Original Author:
# 2012 Ryan Pavlik <rpavlik@iastate.edu> <abiryan@ryand.net>
# http://academic.cleardefinition.com
# Iowa State University HCI Graduate Program/VRAC
#
# Copyright Iowa State University 2012.
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)

set(_preferred_sdk_dirs) # pre-output
set(_win_sdk_dirs) # pre-output
set(_win_sdk_versanddirs) # pre-output
set(_win_sdk_buildsanddirs) # pre-output
set(_winsdk_vistaonly) # search parameters
set(_winsdk_kits) # search parameters


set(_WINDOWSSDK_ANNOUNCE OFF)
if(NOT WINDOWSSDK_FOUND AND (NOT WindowsSDK_FIND_QUIETLY))
	set(_WINDOWSSDK_ANNOUNCE ON)
endif()
macro(_winsdk_announce)
	if(_WINSDK_ANNOUNCE)
		message(STATUS ${ARGN})
	endif()
endmacro()


set(_winsdk_win10vers
	10.0.18362.0 # Windows 10 SDK for 2019 Update
  	10.0.17763.0 # Windows 10 SDK for October 2018 Update
	10.0.17133.0 # Redstone 4 aka Win10 1803 "April 1018 Update"
	10.0.16299.0 # Redstone 3 aka Win10 1709 "Fall Creators Update"
	10.0.15063.0 # Redstone 2 aka Win10 1703 "Creators Update"
	10.0.14393.0 # Redstone aka Win10 1607 "Anniversary Update"
	10.0.10586.0 # TH2 aka Win10 1511
	10.0.10240.0 # Win10 RTM
	10.0.10150.0 # just ucrt
	10.0.10056.0
)

if(WindowsSDK_FIND_COMPONENTS MATCHES "tools")
	set(_WINDOWSSDK_IGNOREMSVC ON)
	_winsdk_announce("Checking for tools from Windows/Platform SDKs...")
else()
	set(_WINDOWSSDK_IGNOREMSVC OFF)
	_winsdk_announce("Checking for Windows/Platform SDKs...")
endif()

# Appends to the three main pre-output lists used only if the path exists
# and is not already in the list.
function(_winsdk_conditional_append _vername _build _path)
	if(("${_path}" MATCHES "registry") OR (NOT EXISTS "${_path}"))
		# Path invalid - do not add
		return()
	endif()
	list(FIND _win_sdk_dirs "${_path}" _win_sdk_idx)
	if(_win_sdk_idx GREATER -1)
		# Path already in list - do not add
		return()
	endif()
	_winsdk_announce( " - ${_vername}, Build ${_build} @ ${_path}")
	# Not yet in the list, so we'll add it
	list(APPEND _win_sdk_dirs "${_path}")
	set(_win_sdk_dirs "${_win_sdk_dirs}" CACHE INTERNAL "" FORCE)
	list(APPEND
		_win_sdk_versanddirs
		"${_vername}"
		"${_path}")
	set(_win_sdk_versanddirs "${_win_sdk_versanddirs}" CACHE INTERNAL "" FORCE)
	list(APPEND
		_win_sdk_buildsanddirs
		"${_build}"
		"${_path}")
	set(_win_sdk_buildsanddirs "${_win_sdk_buildsanddirs}" CACHE INTERNAL "" FORCE)
endfunction()

# Appends to the "preferred SDK" lists only if the path exists
function(_winsdk_conditional_append_preferred _info _path)
	if(("${_path}" MATCHES "registry") OR (NOT EXISTS "${_path}"))
		# Path invalid - do not add
		return()
	endif()

	get_filename_component(_path "${_path}" ABSOLUTE)

	list(FIND _win_sdk_preferred_sdk_dirs "${_path}" _win_sdk_idx)
	if(_win_sdk_idx GREATER -1)
		# Path already in list - do not add
		return()
	endif()
	_winsdk_announce( " - Found \"preferred\" SDK ${_info} @ ${_path}")
	# Not yet in the list, so we'll add it
	list(APPEND _win_sdk_preferred_sdk_dirs "${_path}")
	set(_win_sdk_preferred_sdk_dirs "${_win_sdk_dirs}" CACHE INTERNAL "" FORCE)

	# Just in case we somehow missed it:
	_winsdk_conditional_append("${_info}" "" "${_path}")
endfunction()

# Given a version like v7.0A, looks for an SDK in the registry under "Microsoft SDKs".
# If the given version might be in both HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows
# and HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots aka "Windows Kits",
# use this macro first, since these registry keys usually have more information.
#
# Pass a "default" build number as an extra argument in case we can't find it.
function(_winsdk_check_microsoft_sdks_registry _winsdkver)
	set(SDKKEY "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows\\${_winsdkver}")
	get_filename_component(_sdkdir
		"[${SDKKEY};InstallationFolder]"
		ABSOLUTE)

	set(_sdkname "Windows SDK ${_winsdkver}")

	# Default build number passed as extra argument
	set(_build ${ARGN})
	# See if the registry holds a Microsoft-mutilated, err, designated, product name
	# (just using get_filename_component to execute the registry lookup)
	get_filename_component(_sdkproductname
		"[${SDKKEY};ProductName]"
		NAME)
	if(NOT "${_sdkproductname}" MATCHES "registry")
		# Got a product name
		set(_sdkname "${_sdkname} (${_sdkproductname})")
	endif()

	# try for a version to augment our name
	# (just using get_filename_component to execute the registry lookup)
	get_filename_component(_sdkver
		"[${SDKKEY};ProductVersion]"
		NAME)
	if(NOT "${_sdkver}" MATCHES "registry" AND NOT MATCHES)
		# Got a version
		if(NOT "${_sdkver}" MATCHES "\\.\\.")
			# and it's not an invalid one with two dots in it:
			# use to override the default build
			set(_build ${_sdkver})
			if(NOT "${_sdkname}" MATCHES "${_sdkver}")
				# Got a version that's not already in the name, let's use it to improve our name.
				set(_sdkname "${_sdkname} (${_sdkver})")
			endif()
		endif()
	endif()
	_winsdk_conditional_append("${_sdkname}" "${_build}" "${_sdkdir}")
endfunction()

# Given a name for identification purposes, the build number, and a key (technically a "value name")
# corresponding to a Windows SDK packaged as a "Windows Kit", look for it
# in HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots
# Note that the key or "value name" tends to be something weird like KitsRoot81 -
# no easy way to predict, just have to observe them in the wild.
# Doesn't hurt to also try _winsdk_check_microsoft_sdks_registry for these:
# sometimes you get keys in both parts of the registry (in the wow64 portion especially),
# and the non-"Windows Kits" location is often more descriptive.
function(_winsdk_check_windows_kits_registry _winkit_name _winkit_build _winkit_key)
	get_filename_component(_sdkdir
		"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;${_winkit_key}]"
		ABSOLUTE)
	_winsdk_conditional_append("${_winkit_name}" "${_winkit_build}" "${_sdkdir}")
endfunction()

# Given a name for identification purposes and the build number
# corresponding to a Windows 10 SDK packaged as a "Windows Kit", look for it
# in HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots
# Doesn't hurt to also try _winsdk_check_microsoft_sdks_registry for these:
# sometimes you get keys in both parts of the registry (in the wow64 portion especially),
# and the non-"Windows Kits" location is often more descriptive.
function(_winsdk_check_win10_kits _winkit_build)
	get_filename_component(_sdkdir
		"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
		ABSOLUTE)
	if(("${_sdkdir}" MATCHES "registry") OR (NOT EXISTS "${_sdkdir}"))
		return() # not found
	endif()
	if(EXISTS "${_sdkdir}/Include/${_winkit_build}/um")
		_winsdk_conditional_append("Windows Kits 10 (Build ${_winkit_build})" "${_winkit_build}" "${_sdkdir}")
	endif()
endfunction()

# Given a name for identification purposes, the build number, and the associated package GUID,
# look in the registry under both HKLM and HKCU in \\SOFTWARE\\Microsoft\\MicrosoftSDK\\InstalledSDKs\\
# for that guid and the SDK it points to.
function(_winsdk_check_platformsdk_registry _platformsdkname _build _platformsdkguid)
	foreach(_winsdk_hive HKEY_LOCAL_MACHINE HKEY_CURRENT_USER)
		get_filename_component(_sdkdir
			"[${_winsdk_hive}\\SOFTWARE\\Microsoft\\MicrosoftSDK\\InstalledSDKs\\${_platformsdkguid};Install Dir]"
			ABSOLUTE)
		_winsdk_conditional_append("${_platformsdkname} (${_build})" "${_build}" "${_sdkdir}")
	endforeach()
endfunction()

###
# Detect toolchain information: to know whether it's OK to use Vista+ only SDKs
###
set(_winsdk_vistaonly_ok OFF)
if(MSVC AND NOT _WINDOWSSDK_IGNOREMSVC)
	# VC 10 and older has broad target support
	if(MSVC_VERSION LESS 1700)
		# VC 11 by default targets Vista and later only, so we can add a few more SDKs that (might?) only work on vista+
	elseif("${CMAKE_VS_PLATFORM_TOOLSET}" MATCHES "_xp")
		# This is the XP-compatible v110+ toolset
	elseif("${CMAKE_VS_PLATFORM_TOOLSET}" STREQUAL "v100" OR "${CMAKE_VS_PLATFORM_TOOLSET}" STREQUAL "v90")
		# This is the VS2010/VS2008 toolset
	else()
		# OK, we're VC11 or newer and not using a backlevel or XP-compatible toolset.
		# These versions have no XP (and possibly Vista pre-SP1) support
		set(_winsdk_vistaonly_ok ON)
		if(_WINDOWSSDK_ANNOUNCE AND NOT _WINDOWSSDK_VISTAONLY_PESTERED)
			set(_WINDOWSSDK_VISTAONLY_PESTERED ON CACHE INTERNAL "" FORCE)
			message(STATUS "FindWindowsSDK: Detected Visual Studio 2012 or newer, not using the _xp toolset variant: including SDK versions that drop XP support in search!")
		endif()
	endif()
endif()
if(_WINDOWSSDK_IGNOREMSVC)
	set(_winsdk_vistaonly_ok ON)
endif()

###
# MSVC version checks - keeps messy conditionals in one place
# (messy because of _WINDOWSSDK_IGNOREMSVC)
###
set(_winsdk_msvc_greater_1200 OFF)
if(_WINDOWSSDK_IGNOREMSVC OR (MSVC AND (MSVC_VERSION GREATER 1200)))
	set(_winsdk_msvc_greater_1200 ON)
endif()
# Newer than VS .NET/VS Toolkit 2003
set(_winsdk_msvc_greater_1310 OFF)
if(_WINDOWSSDK_IGNOREMSVC OR (MSVC AND (MSVC_VERSION GREATER 1310)))
	set(_winsdk_msvc_greater_1310 ON)
endif()

# VS2005/2008
set(_winsdk_msvc_less_1600 OFF)
if(_WINDOWSSDK_IGNOREMSVC OR (MSVC AND (MSVC_VERSION LESS 1600)))
	set(_winsdk_msvc_less_1600 ON)
endif()

# VS2013+
set(_winsdk_msvc_not_less_1800 OFF)
if(_WINDOWSSDK_IGNOREMSVC OR (MSVC AND (NOT MSVC_VERSION LESS 1800)))
	set(_winsdk_msvc_not_less_1800 ON)
endif()

###
# START body of find module
###
if(_winsdk_msvc_greater_1310) # Newer than VS .NET/VS Toolkit 2003
	###
	# Look for "preferred" SDKs
	###

	# Environment variable for SDK dir
	if(EXISTS "$ENV{WindowsSDKDir}" AND (NOT "$ENV{WindowsSDKDir}" STREQUAL ""))
		_winsdk_conditional_append_preferred("WindowsSDKDir environment variable" "$ENV{WindowsSDKDir}")
	endif()

	if(_winsdk_msvc_less_1600)
		# Per-user current Windows SDK for VS2005/2008
		get_filename_component(_sdkdir
			"[HKEY_CURRENT_USER\\Software\\Microsoft\\Microsoft SDKs\\Windows;CurrentInstallFolder]"
			ABSOLUTE)
		_winsdk_conditional_append_preferred("Per-user current Windows SDK" "${_sdkdir}")

		# System-wide current Windows SDK for VS2005/2008
		get_filename_component(_sdkdir
			"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows;CurrentInstallFolder]"
			ABSOLUTE)
		_winsdk_conditional_append_preferred("System-wide current Windows SDK" "${_sdkdir}")
	endif()

	###
	# Begin the massive list of SDK searching!
	###
	if(_winsdk_vistaonly_ok AND _winsdk_msvc_not_less_1800)
		# These require at least Visual Studio 2013 (VC12)

		_winsdk_check_microsoft_sdks_registry(v10.0A)

		# Windows Software Development Kit (SDK) for Windows 10
		# Several different versions living in the same directory - if nothing else we can assume RTM (10240)
		_winsdk_check_microsoft_sdks_registry(v10.0 10.0.10240.0)
		foreach(_win10build ${_winsdk_win10vers})
			_winsdk_check_win10_kits(${_win10build})
		endforeach()
	endif() # vista-only and 2013+

	# Included in Visual Studio 2013
	# Includes the v120_xp toolset
	_winsdk_check_microsoft_sdks_registry(v8.1A 8.1.51636)

	if(_winsdk_vistaonly_ok AND _winsdk_msvc_not_less_1800)
		# Windows Software Development Kit (SDK) for Windows 8.1
		# http://msdn.microsoft.com/en-gb/windows/desktop/bg162891
		_winsdk_check_microsoft_sdks_registry(v8.1 8.1.25984.0)
		_winsdk_check_windows_kits_registry("Windows Kits 8.1" 8.1.25984.0 KitsRoot81)
	endif() # vista-only and 2013+

	if(_winsdk_vistaonly_ok)
		# Included in Visual Studio 2012
		_winsdk_check_microsoft_sdks_registry(v8.0A 8.0.50727)

		# Microsoft Windows SDK for Windows 8 and .NET Framework 4.5
		# This is the first version to also include the DirectX SDK
		# http://msdn.microsoft.com/en-US/windows/desktop/hh852363.aspx
		_winsdk_check_microsoft_sdks_registry(v8.0 6.2.9200.16384)
		_winsdk_check_windows_kits_registry("Windows Kits 8.0" 6.2.9200.16384 KitsRoot)
	endif() # vista-only

	# Included with VS 2012 Update 1 or later
	# Introduces v110_xp toolset
	_winsdk_check_microsoft_sdks_registry(v7.1A 7.1.51106)
	if(_winsdk_vistaonly_ok)
		# Microsoft Windows SDK for Windows 7 and .NET Framework 4
		# http://www.microsoft.com/downloads/en/details.aspx?FamilyID=6b6c21d2-2006-4afa-9702-529fa782d63b
		_winsdk_check_microsoft_sdks_registry(v7.1 7.1.7600.0.30514)
	endif() # vista-only

	# Included with VS 2010
	_winsdk_check_microsoft_sdks_registry(v7.0A 6.1.7600.16385)

	# Windows SDK for Windows 7 and .NET Framework 3.5 SP1
	# Works with VC9
	# http://www.microsoft.com/en-us/download/details.aspx?id=18950
	_winsdk_check_microsoft_sdks_registry(v7.0 6.1.7600.16385)

	# Two versions call themselves "v6.1":
	# Older:
	# Windows Vista Update & .NET 3.0 SDK
	# http://www.microsoft.com/en-us/download/details.aspx?id=14477

	# Newer:
	# Windows Server 2008 & .NET 3.5 SDK
	# may have broken VS9SP1? they recommend v7.0 instead, or a KB...
	# http://www.microsoft.com/en-us/download/details.aspx?id=24826
	_winsdk_check_microsoft_sdks_registry(v6.1 6.1.6000.16384.10)

	# Included in VS 2008
	_winsdk_check_microsoft_sdks_registry(v6.0A 6.1.6723.1)

	# Microsoft Windows Software Development Kit for Windows Vista and .NET Framework 3.0 Runtime Components
	# http://blogs.msdn.com/b/stanley/archive/2006/11/08/microsoft-windows-software-development-kit-for-windows-vista-and-net-framework-3-0-runtime-components.aspx
	_winsdk_check_microsoft_sdks_registry(v6.0 6.0.6000.16384)
endif()

# Let's not forget the Platform SDKs, which sometimes are useful!
if(_winsdk_msvc_greater_1200)
	_winsdk_check_platformsdk_registry("Microsoft Platform SDK for Windows Server 2003 R2" "5.2.3790.2075.51" "D2FF9F89-8AA2-4373-8A31-C838BF4DBBE1")
	_winsdk_check_platformsdk_registry("Microsoft Platform SDK for Windows Server 2003 SP1" "5.2.3790.1830.15" "8F9E5EF3-A9A5-491B-A889-C58EFFECE8B3")
endif()
###
# Finally, look for "preferred" SDKs
###
if(_winsdk_msvc_greater_1310) # Newer than VS .NET/VS Toolkit 2003


	# Environment variable for SDK dir
	if(EXISTS "$ENV{WindowsSDKDir}" AND (NOT "$ENV{WindowsSDKDir}" STREQUAL ""))
		_winsdk_conditional_append_preferred("WindowsSDKDir environment variable" "$ENV{WindowsSDKDir}")
	endif()

	if(_winsdk_msvc_less_1600)
		# Per-user current Windows SDK for VS2005/2008
		get_filename_component(_sdkdir
			"[HKEY_CURRENT_USER\\Software\\Microsoft\\Microsoft SDKs\\Windows;CurrentInstallFolder]"
			ABSOLUTE)
		_winsdk_conditional_append_preferred("Per-user current Windows SDK" "${_sdkdir}")

		# System-wide current Windows SDK for VS2005/2008
		get_filename_component(_sdkdir
			"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows;CurrentInstallFolder]"
			ABSOLUTE)
		_winsdk_conditional_append_preferred("System-wide current Windows SDK" "${_sdkdir}")
	endif()
endif()


function(windowssdk_name_lookup _dir _outvar)
	list(FIND _win_sdk_versanddirs "${_dir}" _diridx)
	math(EXPR _idx "${_diridx} - 1")
	if(${_idx} GREATER -1)
		list(GET _win_sdk_versanddirs ${_idx} _ret)
	else()
		set(_ret "NOTFOUND")
	endif()
	set(${_outvar} "${_ret}" PARENT_SCOPE)
endfunction()

function(windowssdk_build_lookup _dir _outvar)
	list(FIND _win_sdk_buildsanddirs "${_dir}" _diridx)
	math(EXPR _idx "${_diridx} - 1")
	if(${_idx} GREATER -1)
		list(GET _win_sdk_buildsanddirs ${_idx} _ret)
	else()
		set(_ret "NOTFOUND")
	endif()
	set(${_outvar} "${_ret}" PARENT_SCOPE)
endfunction()

# If we found something...
if(_win_sdk_dirs)
	list(GET _win_sdk_dirs 0 WINDOWSSDK_LATEST_DIR)
	windowssdk_name_lookup("${WINDOWSSDK_LATEST_DIR}"
		WINDOWSSDK_LATEST_NAME)
	set(WINDOWSSDK_DIRS ${_win_sdk_dirs})

	# Fallback, in case no preference found.
	set(WINDOWSSDK_PREFERRED_DIR "${WINDOWSSDK_LATEST_DIR}")
	set(WINDOWSSDK_PREFERRED_NAME "${WINDOWSSDK_LATEST_NAME}")
	set(WINDOWSSDK_PREFERRED_FIRST_DIRS ${WINDOWSSDK_DIRS})
	set(WINDOWSSDK_FOUND_PREFERENCE OFF)
endif()

# If we found indications of a user preference...
if(_win_sdk_preferred_sdk_dirs)
	list(GET _win_sdk_preferred_sdk_dirs 0 WINDOWSSDK_PREFERRED_DIR)
	windowssdk_name_lookup("${WINDOWSSDK_PREFERRED_DIR}"
		WINDOWSSDK_PREFERRED_NAME)
	set(WINDOWSSDK_PREFERRED_FIRST_DIRS
		${_win_sdk_preferred_sdk_dirs}
		${_win_sdk_dirs})
	list(REMOVE_DUPLICATES WINDOWSSDK_PREFERRED_FIRST_DIRS)
	set(WINDOWSSDK_FOUND_PREFERENCE ON)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WindowsSDK
	"No compatible version of the Windows SDK or Platform SDK found."
	WINDOWSSDK_DIRS)

if(WINDOWSSDK_FOUND)
	# Internal: Architecture-appropriate library directory names.
	if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "ARM")
		if(CMAKE_SIZEOF_VOID_P MATCHES "8")
			# Only supported in Win10 SDK and up.
			set(_winsdk_arch8 arm64) # what the WDK for Win8+ calls this architecture
		else()
			set(_winsdk_archbare /arm) # what the architecture used to be called in oldest SDKs
			set(_winsdk_arch arm) # what the architecture used to be called
			set(_winsdk_arch8 arm) # what the WDK for Win8+ calls this architecture
		endif()
	else()
		if(CMAKE_SIZEOF_VOID_P MATCHES "8")
			set(_winsdk_archbare /x64) # what the architecture used to be called in oldest SDKs
			set(_winsdk_arch amd64) # what the architecture used to be called
			set(_winsdk_arch8 x64) # what the WDK for Win8+ calls this architecture
		else()
			set(_winsdk_archbare ) # what the architecture used to be called in oldest SDKs
			set(_winsdk_arch i386) # what the architecture used to be called
			set(_winsdk_arch8 x86) # what the WDK for Win8+ calls this architecture
		endif()
	endif()

	function(get_windowssdk_from_component _component _var)
		get_filename_component(_component "${_component}" ABSOLUTE)
		file(TO_CMAKE_PATH "${_component}" _component)
		foreach(_sdkdir ${WINDOWSSDK_DIRS})
			get_filename_component(_sdkdir "${_sdkdir}" ABSOLUTE)
			string(LENGTH "${_sdkdir}" _sdklen)
			file(RELATIVE_PATH _rel "${_sdkdir}" "${_component}")
			# If we don't have any "parent directory" items...
			if(NOT "${_rel}" MATCHES "[.][.]")
				set(${_var} "${_sdkdir}" PARENT_SCOPE)
				return()
			endif()
		endforeach()
		# Fail.
		set(${_var} "NOTFOUND" PARENT_SCOPE)
	endfunction()
	function(get_windowssdk_library_dirs _winsdk_dir _var)
		set(_dirs)
		set(_suffixes
			"lib${_winsdk_archbare}" # SDKs like 7.1A
			"lib/${_winsdk_arch}" # just because some SDKs have x86 dir and root dir
			"lib/w2k/${_winsdk_arch}" # Win2k min requirement
			"lib/wxp/${_winsdk_arch}" # WinXP min requirement
			"lib/wnet/${_winsdk_arch}" # Win Server 2003 min requirement
			"lib/wlh/${_winsdk_arch}"
			"lib/wlh/um/${_winsdk_arch8}" # Win Vista ("Long Horn") min requirement
			"lib/win7/${_winsdk_arch}"
			"lib/win7/um/${_winsdk_arch8}" # Win 7 min requirement
		)
		foreach(_ver
			wlh # Win Vista ("Long Horn") min requirement
			win7 # Win 7 min requirement
			win8 # Win 8 min requirement
			winv6.3 # Win 8.1 min requirement
		)

			list(APPEND _suffixes
				"lib/${_ver}/${_winsdk_arch}"
				"lib/${_ver}/um/${_winsdk_arch8}"
				"lib/${_ver}/km/${_winsdk_arch8}"
			)
		endforeach()

		# Look for WDF libraries in Win10+ SDK
		foreach(_mode umdf kmdf)
			file(GLOB _wdfdirs RELATIVE "${_winsdk_dir}" "${_winsdk_dir}/lib/wdf/${_mode}/${_winsdk_arch8}/*")
			if(_wdfdirs)
				list(APPEND _suffixes ${_wdfdirs})
			endif()
		endforeach()

		# Look in each Win10+ SDK version for the components
		foreach(_win10ver ${_winsdk_win10vers})
			foreach(_component um km ucrt mmos)
				list(APPEND _suffixes "lib/${_win10ver}/${_component}/${_winsdk_arch8}")
			endforeach()
		endforeach()

		foreach(_suffix ${_suffixes})
			# Check to see if a library actually exists here.
			file(GLOB _libs "${_winsdk_dir}/${_suffix}/*.lib")
			if(_libs)
				list(APPEND _dirs "${_winsdk_dir}/${_suffix}")
			endif()
		endforeach()
		if("${_dirs}" STREQUAL "")
			set(_dirs NOTFOUND)
		else()
			list(REMOVE_DUPLICATES _dirs)
		endif()
		set(${_var} ${_dirs} PARENT_SCOPE)
	endfunction()
	function(get_windowssdk_include_dirs _winsdk_dir _var)
		set(_dirs)

		set(_subdirs shared um winrt km wdf mmos ucrt)
		set(_suffixes Include)

		foreach(_dir ${_subdirs})
			list(APPEND _suffixes "Include/${_dir}")
		endforeach()

		foreach(_ver ${_winsdk_win10vers})
			foreach(_dir ${_subdirs})
				list(APPEND _suffixes "Include/${_ver}/${_dir}")
			endforeach()
		endforeach()

		foreach(_suffix ${_suffixes})
			# Check to see if a header file actually exists here.
			file(GLOB _headers "${_winsdk_dir}/${_suffix}/*.h")
			if(_headers)
				list(APPEND _dirs "${_winsdk_dir}/${_suffix}")
			endif()
		endforeach()
		if("${_dirs}" STREQUAL "")
			set(_dirs NOTFOUND)
		else()
			list(REMOVE_DUPLICATES _dirs)
		endif()
		set(${_var} ${_dirs} PARENT_SCOPE)
	endfunction()



	function(get_windowssdk_library_dirs_multiple _var)
		set(_dirs)
		foreach(_sdkdir ${ARGN})
			get_windowssdk_library_dirs("${_sdkdir}" _current_sdk_libdirs)
			if(_current_sdk_libdirs)
				list(APPEND _dirs ${_current_sdk_libdirs})
			endif()
		endforeach()
		if("${_dirs}" STREQUAL "")
			set(_dirs NOTFOUND)
		else()
			list(REMOVE_DUPLICATES _dirs)
		endif()
		set(${_var} ${_dirs} PARENT_SCOPE)
	endfunction()
	function(get_windowssdk_include_dirs_multiple _var)
		set(_dirs)
		foreach(_sdkdir ${ARGN})
			get_windowssdk_include_dirs("${_sdkdir}" _current_sdk_incdirs)
			if(_current_sdk_libdirs)
				list(APPEND _dirs ${_current_sdk_incdirs})
			endif()
		endforeach()
		if("${_dirs}" STREQUAL "")
			set(_dirs NOTFOUND)
		else()
			list(REMOVE_DUPLICATES _dirs)
		endif()
		set(${_var} ${_dirs} PARENT_SCOPE)
	endfunction()
endif()


function(FindFirstStringMatching list reg matching)
  foreach(l ${${list}})
    if(${l} MATCHES ${reg})
      set(${matching} ${l} PARENT_SCOPE)
      break()
    endif()
  endforeach()
endfunction()

function(GetUMWindowsSDKLibraryDir library_dir)
  get_windowssdk_library_dirs(${WINDOWSSDK_LATEST_DIR} WIN_LIBRARY_DIRS)
  FindFirstStringMatching(WIN_LIBRARY_DIRS "[\\/]um[\\/]" WINDOWSKIT_LIBRARY_DIR)
  set(${library_dir} ${WINDOWSKIT_LIBRARY_DIR} PARENT_SCOPE)
endfunction()

function(GetUMWindowsSDKIncludeDir include_dir)
  get_windowssdk_include_dirs(${WINDOWSSDK_LATEST_DIR} WIN_INCLUDE_DIRS)
  FindFirstStringMatching(WIN_INCLUDE_DIRS "[\\/]um[\\/]" WIN_INCLUDE_DIR)
  set(${include_dir} ${WIN_INCLUDE_DIR} PARENT_SCOPE)
endfunction()
