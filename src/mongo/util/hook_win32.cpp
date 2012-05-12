// @file hook_win32.cpp : Used to hook Windows functions imported through the Import Address Table

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

#if defined(_WIN32)

#include "mongo/util/hook_win32.h"

#include "mongo/platform/windows_basic.h"

#include <DbgHelp.h>

#include "mongo/util/assert_util.h"

#pragma comment(lib, "dbghelp.lib")

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
            void* hookFunction ) {

        if ( hookModuleAddress == 0 ) {
            return NULL;
        }

        // look up the original function by module and function name
        HMODULE moduleHandle = GetModuleHandleA( functionModuleName );
        void* originalFunction = GetProcAddress( moduleHandle, functionName );
        if ( originalFunction == 0 ) {
            return NULL;
        }

        // get a pointer to this module's Import Table
        ULONG entrySize;
        void* imageEntry = ImageDirectoryEntryToDataEx(
                hookModuleAddress,              // address of module with IAT to patch
                TRUE,                           // mapped as image
                IMAGE_DIRECTORY_ENTRY_IMPORT,   // desired directory
                &entrySize,                     // returned directory size
                NULL                            // returned section ptr (always zero for IAT)
        );
        IMAGE_IMPORT_DESCRIPTOR* importModule =
                reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>( imageEntry );

        // walk the imported module list until we find the desired module
        for ( ; importModule->Name; ++importModule ) {
            char* foundModuleName = hookModuleAddress + importModule->Name;
            if ( _strcmpi( foundModuleName, functionModuleName ) == 0 ) {

                // search the list of imported functions for the address of the one we want to hook
                IMAGE_THUNK_DATA* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                        hookModuleAddress + importModule->FirstThunk );
                for ( ; thunk->u1.Function; ++thunk ) {
                    if ( reinterpret_cast<void*>( thunk->u1.Function ) == originalFunction ) {

                        // we found our function, remember its location and then hook it
                        void** _tablePointer = reinterpret_cast<void**>( &thunk->u1.Function );
                        MEMORY_BASIC_INFORMATION mbi;
                        VirtualQuery( _tablePointer, &mbi, sizeof(mbi) );
                        fassert( 16239, VirtualProtect( mbi.BaseAddress,
                                                        mbi.RegionSize,
                                                        PAGE_EXECUTE_READWRITE,
                                                        &mbi.Protect ) );
                        *_tablePointer = hookFunction;
                        DWORD unused;
                        fassert( 16240, VirtualProtect( mbi.BaseAddress,
                                                        mbi.RegionSize,
                                                        mbi.Protect,
                                                        &unused ) );
                        return originalFunction;
                    }
                }
                break;
            }
        }
        return NULL;
    }

} // namespace mongo

#endif // #if defined(_WIN32)
