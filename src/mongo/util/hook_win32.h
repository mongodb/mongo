// @file hook_win32.h : Used to hook Windows functions imported through the Import Address Table

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#if defined(_WIN32)

namespace mongo {

    /**
     * Hook a Windows API through the Import Address Table of a calling module
     *
     * @param hookModuleAddress     ptr to start of importing executable
     * @param functionModuleName    name of module containing function to hook
     * @param functionName          name of function to hook
     * @param hookFunction          ptr to replacement (hook) function
     * @return                      ptr to original (hooked) function
     */
    void* hookWin32(
        char* hookModuleAddress,
        char* functionModuleName,
        char* functionName,
        void* hookFunction );

} // namespace mongo

#endif // #if defined(_WIN32)
