// Copyright 2009.  10gen, Inc.

#include "mongo/util/stacktrace.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <map>
#include <vector>

#ifdef _WIN32
#include <boost/smart_ptr/scoped_array.hpp>
#include "mongo/platform/windows_basic.h"
#include <DbgHelp.h>
#endif

#ifdef MONGO_HAVE_EXECINFO_BACKTRACE

#include <execinfo.h>

namespace mongo {
    static const int maxBackTraceFrames = 20;
    
    void printStackTrace( std::ostream &os ) {
        
        void *b[maxBackTraceFrames];
        
        int size = ::backtrace( b, maxBackTraceFrames );
        for ( int i = 0; i < size; i++ )
            os << std::hex << b[i] << std::dec << ' ';
        os << std::endl;
        
        char **strings;
        
        strings = ::backtrace_symbols( b, size );
        for ( int i = 0; i < size; i++ )
            os << ' ' << strings[i] << '\n';
        os.flush();
        ::free( strings );
    }
}

#elif defined(_WIN32)

namespace mongo {

    struct TraceItem {
        std::string module;
        std::string sourceFile;
        std::string symbol;
        size_t lineNumber;
        size_t instructionOffset;
        size_t sourceLength;
    };

    static const int maxBackTraceFrames = 20;

    void printStackTrace( std::ostream &os ) {
        HANDLE process = GetCurrentProcess();
        BOOL ret = SymInitialize( process, NULL, TRUE );
        if ( ret == FALSE ) {
            DWORD dosError = GetLastError();
            os << "Stack trace failed, SymInitialize failed with error " <<
                    std::dec << dosError << std::endl;
            return;
        }
        DWORD options = SymGetOptions();
        options |= SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS;
        SymSetOptions( options );

        CONTEXT context;
        memset( &context, 0, sizeof(context) );
        context.ContextFlags = CONTEXT_CONTROL;
        RtlCaptureContext( &context );

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
        const size_t symbolRecordSize = sizeof(SYMBOL_INFO) + nameSize;
        boost::scoped_array<char> symbolBuffer( new char[symbolRecordSize] );
        memset( symbolBuffer.get(), 0, symbolRecordSize );
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>( symbolBuffer.get() );
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = nameSize;

        std::vector<TraceItem> traceList;
        TraceItem traceItem;
        size_t moduleWidth = 0;
        size_t fileWidth = 0;
        size_t frameCount = 0;
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
                frameCount = i;
                break;
            }

            // module (executable) name
            IMAGEHLP_MODULE64 module;
            memset ( &module, 0, sizeof(module) );
            module.SizeOfStruct = sizeof(module);
            ret = SymGetModuleInfo64( process, frame64.AddrPC.Offset, &module );
            char* moduleName = module.LoadedImageName;
            char* backslash = strrchr( moduleName, '\\' );
            if ( backslash ) {
                moduleName = backslash + 1;
            }
            traceItem.module = moduleName;
            size_t len = traceItem.module.length();
            if ( len > moduleWidth ) {
                moduleWidth = len;
            }

            // source code filename and line number
            IMAGEHLP_LINE64 line;
            memset( &line, 0, sizeof(line) );
            line.SizeOfStruct = sizeof(line);
            DWORD displacement32;
            ret = SymGetLineFromAddr64( process, frame64.AddrPC.Offset, &displacement32, &line );
            if ( ret ) {
                std::string filename( line.FileName );
                std::string::size_type start = filename.find( "\\src\\mongo\\" );
                if ( start == std::string::npos ) {
                    start = filename.find( "\\src\\third_party\\" );
                }
                if ( start != std::string::npos ) {
                    std::string shorter( "..." );
                    shorter += filename.substr( start );
                    traceItem.sourceFile.swap( shorter );
                }
                else {
                    traceItem.sourceFile.swap( filename );
                }
                len = traceItem.sourceFile.length() + 3;
                traceItem.lineNumber = line.LineNumber;
                if ( traceItem.lineNumber < 10 ) {
                    len += 1;
                }
                else if ( traceItem.lineNumber < 100 ) {
                    len += 2;
                }
                else if ( traceItem.lineNumber < 1000 ) {
                    len += 3;
                }
                else if ( traceItem.lineNumber < 10000 ) {
                    len += 4;
                }
                else {
                    len += 5;
                }
                traceItem.sourceLength = len;
                if ( len > fileWidth ) {
                    fileWidth = len;
                }
            }
            else {
                traceItem.sourceFile.clear();
                traceItem.sourceLength = 0;
            }

            // symbol name and offset from symbol
            DWORD64 displacement;
            ret = SymFromAddr( process, frame64.AddrPC.Offset, &displacement, symbol );
            if ( ret ) {
                traceItem.symbol = symbol->Name;
                traceItem.instructionOffset = displacement;
            }
            else {
                traceItem.symbol = "???";
                traceItem.instructionOffset = 0;
            }

            // add to list
            traceList.push_back( traceItem );
        }
        SymCleanup( process );

        // print list
        ++moduleWidth;
        ++fileWidth;
        for ( size_t i = 0; i < frameCount; ++i ) {
            os << traceList[i].module << " ";
            size_t width = traceList[i].module.length();
            while ( width < moduleWidth ) {
                os << " ";
                ++width;
            }
            if ( traceList[i].sourceFile.length() ) {
                os << traceList[i].sourceFile << "(" << std::dec << traceList[i].lineNumber << ") ";
            }
            width = traceList[i].sourceLength;
            while ( width < fileWidth ) {
                os << " ";
                ++width;
            }
            os << traceList[i].symbol << "+0x" << std::hex << traceList[i].instructionOffset;
            os << std::dec << std::endl;
        }
    }
}

#else

namespace mongo {
    void printStackTrace( std::ostream &os ) {}
}

#endif

