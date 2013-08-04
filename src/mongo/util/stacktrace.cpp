/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/stacktrace.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <map>
#include <vector>

#include "mongo/util/log.h"
#include "mongo/util/concurrency/mutex.h"

#ifdef _WIN32
#include <boost/filesystem/operations.hpp>
#include <boost/smart_ptr/scoped_array.hpp>
#include <sstream>
#include <stdio.h>
#include <DbgHelp.h>
#include "mongo/util/assert_util.h"
#else
#include "mongo/platform/backtrace.h"
#endif

#if defined(_WIN32)

namespace mongo {

    /**
     * Get the path string to be used when searching for PDB files.
     * 
     * @param process        Process handle
     * @return searchPath    Returned search path string
     */
    static const char* getSymbolSearchPath(HANDLE process) {
        static std::string symbolSearchPath;

        if (symbolSearchPath.empty()) {
            static const size_t bufferSize = 1024;
            boost::scoped_array<char> pathBuffer(new char[bufferSize]);
            GetModuleFileNameA(NULL, pathBuffer.get(), bufferSize);
            boost::filesystem::path exePath(pathBuffer.get());
            symbolSearchPath = exePath.parent_path().string();
            symbolSearchPath += ";C:\\Windows\\System32;C:\\Windows";
        }
        return symbolSearchPath.c_str();
    }

    /**
     * Get the display name of the executable module containing the specified address.
     * 
     * @param process               Process handle
     * @param address               Address to find
     * @param returnedModuleName    Returned module name
     */
    static void getModuleName( HANDLE process, DWORD64 address, std::string* returnedModuleName ) {
        IMAGEHLP_MODULE64 module64;
        memset ( &module64, 0, sizeof(module64) );
        module64.SizeOfStruct = sizeof(module64);
        BOOL ret = SymGetModuleInfo64( process, address, &module64 );
        if ( FALSE == ret ) {
            returnedModuleName->clear();
            return;
        }
        char* moduleName = module64.LoadedImageName;
        char* backslash = strrchr( moduleName, '\\' );
        if ( backslash ) {
            moduleName = backslash + 1;
        }
        *returnedModuleName = moduleName;
    }

    /**
     * Get the display name and line number of the source file containing the specified address.
     * 
     * @param process               Process handle
     * @param address               Address to find
     * @param returnedSourceAndLine Returned source code file name with line number
     */
    static void getSourceFileAndLineNumber( HANDLE process,
                                            DWORD64 address,
                                            std::string* returnedSourceAndLine ) {
        IMAGEHLP_LINE64 line64;
        memset( &line64, 0, sizeof(line64) );
        line64.SizeOfStruct = sizeof(line64);
        DWORD displacement32;
        BOOL ret = SymGetLineFromAddr64( process, address, &displacement32, &line64 );
        if ( FALSE == ret ) {
            returnedSourceAndLine->clear();
            return;
        }

        std::string filename( line64.FileName );
        std::string::size_type start = filename.find( "\\src\\mongo\\" );
        if ( start == std::string::npos ) {
            start = filename.find( "\\src\\third_party\\" );
        }
        if ( start != std::string::npos ) {
            std::string shorter( "..." );
            shorter += filename.substr( start );
            filename.swap( shorter );
        }
        static const size_t bufferSize = 32;
        boost::scoped_array<char> lineNumber( new char[bufferSize] );
        _snprintf( lineNumber.get(), bufferSize, "(%u)", line64.LineNumber );
        filename += lineNumber.get();
        returnedSourceAndLine->swap( filename );
    }

    /**
     * Get the display text of the symbol and offset of the specified address.
     * 
     * @param process                   Process handle
     * @param address                   Address to find
     * @param symbolInfo                Caller's pre-built SYMBOL_INFO struct (for efficiency)
     * @param returnedSymbolAndOffset   Returned symbol and offset
     */
    static void getsymbolAndOffset( HANDLE process,
                                    DWORD64 address,
                                    SYMBOL_INFO* symbolInfo,
                                    std::string* returnedSymbolAndOffset ) {
        DWORD64 displacement64;
        BOOL ret = SymFromAddr( process, address, &displacement64, symbolInfo );
        if ( FALSE == ret ) {
            *returnedSymbolAndOffset = "???";
            return;
        }
        std::string symbolString( symbolInfo->Name );
        static const size_t bufferSize = 32;
        boost::scoped_array<char> symbolOffset( new char[bufferSize] );
        _snprintf( symbolOffset.get(), bufferSize, "+0x%x", displacement64 );
        symbolString += symbolOffset.get();
        returnedSymbolAndOffset->swap( symbolString );
    }

    struct TraceItem {
        std::string moduleName;
        std::string sourceAndLine;
        std::string symbolAndOffset;
    };

    static const int maxBackTraceFrames = 20;

    /**
     * Print a stack backtrace for the current thread to the specified ostream.
     * 
     * @param os    ostream& to receive printed stack backtrace
     */
    void printStackTrace( std::ostream& os ) {
        CONTEXT context;
        memset( &context, 0, sizeof(context) );
        context.ContextFlags = CONTEXT_CONTROL;
        RtlCaptureContext( &context );
        printWindowsStackTrace( context, os );
    }

    static SimpleMutex _stackTraceMutex( "stackTraceMutex" );

    /**
     * Print stack trace (using a specified stack context) to "os"
     * 
     * @param context   CONTEXT record for stack trace
     * @param os        ostream& to receive printed stack backtrace
     */
    void printWindowsStackTrace( CONTEXT& context, std::ostream& os ) {
        SimpleMutex::scoped_lock lk(_stackTraceMutex);
        HANDLE process = GetCurrentProcess();
        BOOL ret = SymInitialize(process, getSymbolSearchPath(process), TRUE);
        if ( ret == FALSE ) {
            DWORD dosError = GetLastError();
            log() << "Stack trace failed, SymInitialize failed with error " <<
                    std::dec << dosError << std::endl;
            return;
        }
        DWORD options = SymGetOptions();
        options |= SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS;
        SymSetOptions( options );

        STACKFRAME64 frame64;
        memset( &frame64, 0, sizeof(frame64) );

#if defined(_M_AMD64)
        DWORD imageType = IMAGE_FILE_MACHINE_AMD64;
        frame64.AddrPC.Offset = context.Rip;
        frame64.AddrFrame.Offset = context.Rbp;
        frame64.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
        DWORD imageType = IMAGE_FILE_MACHINE_I386;
        frame64.AddrPC.Offset = context.Eip;
        frame64.AddrFrame.Offset = context.Ebp;
        frame64.AddrStack.Offset = context.Esp;
#else
#error Neither _M_IX86 nor _M_AMD64 were defined
#endif
        frame64.AddrPC.Mode = AddrModeFlat;
        frame64.AddrFrame.Mode = AddrModeFlat;
        frame64.AddrStack.Mode = AddrModeFlat;

        const size_t nameSize = 1024;
        const size_t symbolBufferSize = sizeof(SYMBOL_INFO) + nameSize;
        boost::scoped_array<char> symbolCharBuffer( new char[symbolBufferSize] );
        memset( symbolCharBuffer.get(), 0, symbolBufferSize );
        SYMBOL_INFO* symbolBuffer = reinterpret_cast<SYMBOL_INFO*>( symbolCharBuffer.get() );
        symbolBuffer->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbolBuffer->MaxNameLen = nameSize;

        // build list
        std::vector<TraceItem> traceList;
        TraceItem traceItem;
        size_t moduleWidth = 0;
        size_t sourceWidth = 0;
        for ( size_t i = 0; i < maxBackTraceFrames; ++i ) {
            ret = StackWalk64( imageType,
                               process,
                               GetCurrentThread(),
                               &frame64,
                               &context,
                               NULL,
                               NULL,
                               NULL,
                               NULL );
            if ( ret == FALSE || frame64.AddrReturn.Offset == 0 ) {
                break;
            }
            DWORD64 address = frame64.AddrPC.Offset;
            getModuleName( process, address, &traceItem.moduleName );
            size_t width = traceItem.moduleName.length();
            if ( width > moduleWidth ) {
                moduleWidth = width;
            }
            getSourceFileAndLineNumber( process, address, &traceItem.sourceAndLine );
            width = traceItem.sourceAndLine.length();
            if ( width > sourceWidth ) {
                sourceWidth = width;
            }
            getsymbolAndOffset( process, address, symbolBuffer, &traceItem.symbolAndOffset );
            traceList.push_back( traceItem );
        }
        SymCleanup( process );

        // print list
        ++moduleWidth;
        ++sourceWidth;
        size_t frameCount = traceList.size();
        for ( size_t i = 0; i < frameCount; ++i ) {
            std::stringstream ss;
            ss << traceList[i].moduleName << " ";
            size_t width = traceList[i].moduleName.length();
            while ( width < moduleWidth ) {
                ss << " ";
                ++width;
            }
            ss << traceList[i].sourceAndLine << " ";
            width = traceList[i].sourceAndLine.length();
            while ( width < sourceWidth ) {
                ss << " ";
                ++width;
            }
            ss << traceList[i].symbolAndOffset;
            log() << ss.str() << std::endl;
        }
    }

    // Print error message from C runtime, then fassert
    int crtDebugCallback(int, char* originalMessage, int*) {
        StringData message(originalMessage);
        log() << "*** C runtime error: "
              << message.substr(0, message.find('\n'))
              << ", terminating" << std::endl;
        fassertFailed( 17006 );
    }

}
#else

namespace mongo {
    static const int maxBackTraceFrames = 20;

    /**
     * Print a stack backtrace for the current thread to the specified ostream.
     * 
     * @param os    ostream& to receive printed stack backtrace
     */
    void printStackTrace( std::ostream& os ) {

        void* addresses[maxBackTraceFrames];

        int addressCount = backtrace(addresses, maxBackTraceFrames);
        if (addressCount == 0) {
            const int err = errno;
            os << "Unable to collect backtrace addresses (" << errnoWithDescription(err) << ")"
               << std::endl;
            return;
        }
        for (int i = 0; i < addressCount; i++)
            os << std::hex << addresses[i] << std::dec << ' ';
        os << std::endl;

        char** backtraceStrings = backtrace_symbols(addresses, addressCount);
        if (backtraceStrings == NULL) {
            const int err = errno;
            os << "Unable to collect backtrace symbols (" << errnoWithDescription(err) << ")"
               << std::endl;
            return;
        }
        for (int i = 0; i < addressCount; i++)
            os << ' ' << backtraceStrings[i] << '\n';
        os.flush();
        free(backtraceStrings);
    }
}

#endif
