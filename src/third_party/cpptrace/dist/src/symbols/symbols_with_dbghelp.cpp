#ifdef CPPTRACE_GET_SYMBOLS_WITH_DBGHELP

#include <cpptrace/basic.hpp>
#include <cpptrace/utils.hpp>
#include "symbols/symbols.hpp"
#include "platform/dbghelp_utils.hpp"
#include "binary/object.hpp"
#include "utils/common.hpp"
#include "utils/error.hpp"
#include "utils/utils.hpp"
#include "options.hpp"
#include "logging.hpp"

#include <regex>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace dbghelp {
    // SymFromAddr only returns the function's name. In order to get information about parameters,
    // important for C++ stack traces where functions may be overloaded, we have to manually use
    // Windows DIA to walk debug info structures. Resources:
    // https://web.archive.org/web/20201027025750/http://www.debuginfo.com/articles/dbghelptypeinfo.html
    // https://web.archive.org/web/20201203160805/http://www.debuginfo.com/articles/dbghelptypeinfofigures.html
    // https://github.com/DynamoRIO/dynamorio/blob/master/ext/drsyms/drsyms_windows.c#L1370-L1439
    // TODO: Currently unable to detect rvalue references
    // TODO: Currently unable to detect const
    enum class SymTagEnum {
        SymTagNull, SymTagExe, SymTagCompiland, SymTagCompilandDetails, SymTagCompilandEnv,
        SymTagFunction, SymTagBlock, SymTagData, SymTagAnnotation, SymTagLabel, SymTagPublicSymbol,
        SymTagUDT, SymTagEnum, SymTagFunctionType, SymTagPointerType, SymTagArrayType,
        SymTagBaseType, SymTagTypedef, SymTagBaseClass, SymTagFriend, SymTagFunctionArgType,
        SymTagFuncDebugStart, SymTagFuncDebugEnd, SymTagUsingNamespace, SymTagVTableShape,
        SymTagVTable, SymTagCustom, SymTagThunk, SymTagCustomType, SymTagManagedType,
        SymTagDimension, SymTagCallSite, SymTagInlineSite, SymTagBaseInterface, SymTagVectorType,
        SymTagMatrixType, SymTagHLSLType, SymTagCaller, SymTagCallee, SymTagExport,
        SymTagHeapAllocationSite, SymTagCoffGroup, SymTagMax
    };

    enum class IMAGEHLP_SYMBOL_TYPE_INFO {
        TI_GET_SYMTAG, TI_GET_SYMNAME, TI_GET_LENGTH, TI_GET_TYPE, TI_GET_TYPEID, TI_GET_BASETYPE,
        TI_GET_ARRAYINDEXTYPEID, TI_FINDCHILDREN, TI_GET_DATAKIND, TI_GET_ADDRESSOFFSET,
        TI_GET_OFFSET, TI_GET_VALUE, TI_GET_COUNT, TI_GET_CHILDRENCOUNT, TI_GET_BITPOSITION,
        TI_GET_VIRTUALBASECLASS, TI_GET_VIRTUALTABLESHAPEID, TI_GET_VIRTUALBASEPOINTEROFFSET,
        TI_GET_CLASSPARENTID, TI_GET_NESTED, TI_GET_SYMINDEX, TI_GET_LEXICALPARENT, TI_GET_ADDRESS,
        TI_GET_THISADJUST, TI_GET_UDTKIND, TI_IS_EQUIV_TO, TI_GET_CALLING_CONVENTION,
        TI_IS_CLOSE_EQUIV_TO, TI_GTIEX_REQS_VALID, TI_GET_VIRTUALBASEOFFSET,
        TI_GET_VIRTUALBASEDISPINDEX, TI_GET_IS_REFERENCE, TI_GET_INDIRECTVIRTUALBASECLASS,
        TI_GET_VIRTUALBASETABLETYPE, TI_GET_OBJECTPOINTERTYPE, IMAGEHLP_SYMBOL_TYPE_INFO_MAX
    };

    enum class BasicType {
        btNoType = 0, btVoid = 1, btChar = 2, btWChar = 3, btInt = 6, btUInt = 7, btFloat = 8,
        btBCD = 9, btBool = 10, btLong = 13, btULong = 14, btCurrency = 25, btDate = 26,
        btVariant = 27, btComplex = 28, btBit = 29, btBSTR = 30, btHresult = 31
    };

    // SymGetTypeInfo utility
    template<typename T, IMAGEHLP_SYMBOL_TYPE_INFO SymType, bool FAILABLE = false>
    T get_info(ULONG type_index, HANDLE proc, ULONG64 modbase) {
        T info;
        if(
            !SymGetTypeInfo(
                proc,
                modbase,
                type_index,
                static_cast<::IMAGEHLP_SYMBOL_TYPE_INFO>(SymType),
                &info
            )
        ) {
            if(FAILABLE) {
                return (T)-1;
            } else {
                throw internal_error(
                    "SymGetTypeInfo failed: {}", std::system_error(GetLastError(), std::system_category()).what()
                );
            }
        }
        return info;
    }

    template<IMAGEHLP_SYMBOL_TYPE_INFO SymType, bool FAILABLE = false>
    std::string get_info_wchar(ULONG type_index, HANDLE proc, ULONG64 modbase) {
        WCHAR* info;
        if(
            !SymGetTypeInfo(proc, modbase, type_index, static_cast<::IMAGEHLP_SYMBOL_TYPE_INFO>(SymType), &info)
        ) {
            throw internal_error(
                "SymGetTypeInfo failed: {}", std::system_error(GetLastError(), std::system_category()).what()
            );
        }
        // special case to properly free a buffer and convert string to narrow chars, only used for
        // TI_GET_SYMNAME
        static_assert(
            SymType == IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_SYMNAME,
            "get_info_wchar called with unexpected IMAGEHLP_SYMBOL_TYPE_INFO"
        );
        std::wstring wstr(info);
        std::string str;
        str.reserve(wstr.size());
        for(const auto c : wstr) {
            str.push_back(static_cast<char>(c));
        }
        LocalFree(info);
        return str;
    }

    // Translate basic types to string
    static std::string get_basic_type(ULONG type_index, HANDLE proc, ULONG64 modbase) {
        auto basic_type = get_info<BasicType, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_BASETYPE>(
            type_index,
            proc,
            modbase
        );
        //auto length = get_info<ULONG64, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_LENGTH>(type_index, proc, modbase);
        switch(basic_type) {
            case BasicType::btNoType:
                return "<no basic type>";
            case BasicType::btVoid:
                return "void";
            case BasicType::btChar:
                return "char";
            case BasicType::btWChar:
                return "wchar_t";
            case BasicType::btInt:
                return "int";
            case BasicType::btUInt:
                return "unsigned int";
            case BasicType::btFloat:
                return "float";
            case BasicType::btBool:
                return "bool";
            case BasicType::btLong:
                return "long";
            case BasicType::btULong:
                return "unsigned long";
            default:
                return "<unknown basic type>";
        }
    }

    static std::string resolve_type(ULONG type_index, HANDLE proc, ULONG64 modbase);

    struct class_name_result {
        bool has_class_name;
        std::string name;
    };
    // Helper for member pointers
    static class_name_result lookup_class_name(ULONG type_index, HANDLE proc, ULONG64 modbase) {
        DWORD class_parent_id = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_CLASSPARENTID, true>(
            type_index,
            proc,
            modbase
        );
        if(class_parent_id == (DWORD)-1) {
            return {false, ""};
        } else {
            return {true, resolve_type(class_parent_id, proc, modbase)};
        }
    }

    struct type_result {
        std::string base;
        std::string extent;
    };
    // Resolve more complex types
    // returns [base, extent]
    static type_result lookup_type(ULONG type_index, HANDLE proc, ULONG64 modbase) {
        auto tag = get_info<SymTagEnum, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_SYMTAG>(type_index, proc, modbase);
        switch(tag) {
            case SymTagEnum::SymTagBaseType:
                return {get_basic_type(type_index, proc, modbase), ""};
            case SymTagEnum::SymTagPointerType: {
                DWORD underlying_type_id = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_TYPEID>(
                    type_index,
                    proc,
                    modbase
                );
                bool is_ref = get_info<BOOL, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_IS_REFERENCE>(
                    type_index,
                    proc,
                    modbase
                );
                std::string pp = is_ref ? "&" : "*"; // pointer punctuator
                auto class_name_res = lookup_class_name(type_index, proc, modbase);
                if(class_name_res.has_class_name) {
                    pp = class_name_res.name + "::" + pp;
                }
                const auto type = lookup_type(underlying_type_id, proc, modbase);
                if(type.extent.empty()) {
                    return {type.base + (pp.size() > 1 ? " " : "") + pp, ""};
                } else {
                    return {type.base + "(" + pp, ")" + type.extent};
                }
            }
            case SymTagEnum::SymTagArrayType: {
                DWORD underlying_type_id = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_TYPEID>(
                    type_index,
                    proc,
                    modbase
                );
                DWORD length = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_COUNT>(
                    type_index,
                    proc,
                    modbase
                );
                const auto type = lookup_type(underlying_type_id, proc, modbase);
                return {type.base, "[" + std::to_string(length) + "]" + type.extent};
            }
            case SymTagEnum::SymTagFunctionType: {
                DWORD return_type_id = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_TYPEID>(
                    type_index,
                    proc,
                    modbase
                );
                DWORD n_children = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_COUNT, true>(
                    type_index,
                    proc,
                    modbase
                );
                DWORD class_parent_id = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_CLASSPARENTID, true>(
                    type_index,
                    proc,
                    modbase
                );
                int n_ignore = class_parent_id != (DWORD)-1; // ignore this param
                // this must be ignored before TI_FINDCHILDREN_PARAMS::Count is set, else error
                n_children -= n_ignore;
                // return type
                const auto return_type = lookup_type(return_type_id, proc, modbase);
                if(n_children == 0) {
                    return {return_type.base, "()" + return_type.extent};
                } else {
                    // alignment should be fine
                    std::size_t sz = sizeof(TI_FINDCHILDREN_PARAMS) +
                                    (n_children) * sizeof(TI_FINDCHILDREN_PARAMS::ChildId[0]);
                    TI_FINDCHILDREN_PARAMS* children = (TI_FINDCHILDREN_PARAMS*) new char[sz];
                    auto guard = scope_exit([&] {
                        delete[] (char*) children;
                    });
                    children->Start = 0;
                    children->Count = n_children;
                    if(
                        !SymGetTypeInfo(
                            proc, modbase, type_index,
                            static_cast<::IMAGEHLP_SYMBOL_TYPE_INFO>(
                                IMAGEHLP_SYMBOL_TYPE_INFO::TI_FINDCHILDREN
                            ),
                            children
                        )
                    ) {
                        throw internal_error(
                            "SymGetTypeInfo failed: {}",
                            std::system_error(GetLastError(), std::system_category()).what()
                        );
                    }
                    // get children type
                    std::string extent = "(";
                    if(children->Start != 0) {
                        throw internal_error("Error: children->Start == 0");
                    }
                    for(std::size_t i = 0; i < n_children; i++) {
                        extent += (i == 0 ? "" : ", ") + resolve_type(children->ChildId[i], proc, modbase);
                    }
                    extent += ")";
                    return {return_type.base, extent + return_type.extent};
                }
            }
            case SymTagEnum::SymTagFunctionArgType: {
                DWORD underlying_type_id =
                    get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_TYPEID>(type_index, proc, modbase);
                return {resolve_type(underlying_type_id, proc, modbase), ""};
            }
            case SymTagEnum::SymTagTypedef:
            case SymTagEnum::SymTagEnum:
            case SymTagEnum::SymTagUDT:
            case SymTagEnum::SymTagBaseClass:
                return {
                    get_info_wchar<IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_SYMNAME>(type_index, proc, modbase), ""
                };
            default:
                return {
                    "<unknown type " +
                        std::to_string(static_cast<std::underlying_type<SymTagEnum>::type>(tag)) +
                        ">",
                    ""
                };
        };
    }

    static std::string resolve_type(ULONG type_index, HANDLE proc, ULONG64 modbase) {
        const auto type = lookup_type(type_index, proc, modbase);
        return type.base + type.extent;
    }

    struct function_info {
        HANDLE proc;
        ULONG64 modbase;
        int counter;
        int n_children;
        int n_ignore;
        std::string str;
    };

    // Enumerates function parameters
    static BOOL __stdcall enumerator_callback(
        PSYMBOL_INFO symbol_info,
        ULONG,
        PVOID data
    ) {
        function_info* ctx = (function_info*)data;
        if(ctx->counter++ >= ctx->n_children) {
            return false;
        }
        if(ctx->n_ignore-- > 0) {
            return true; // just skip
        }
        ctx->str += resolve_type(symbol_info->TypeIndex, ctx->proc, ctx->modbase);
        if(ctx->counter < ctx->n_children) {
            ctx->str += ", ";
        }
        return true;
    }

    // TODO: Handle backtrace_pcinfo calling the callback multiple times on inlined functions
    stacktrace_frame resolve_frame(HANDLE proc, frame_ptr addr) {
        // The get_frame_object_info() ends up being inexpensive, at on my machine
        //                                         debug           release
        // uncached trace resolution (29 frames)   1.9-2.1 ms      1.4-1.8 ms
        // cached trace resolution   (29 frames)   1.1-1.2 ms      0.2-0.4 ms
        // get_frame_object_info()                 0.001-0.002 ms  0.0003-0.0006 ms
        // At some point it might make sense to make an option to control this.
        auto object_frame = get_frame_object_info(addr);
        // Dbghelp is is single-threaded, so acquire a lock.
        auto lock = get_dbghelp_lock();
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        union { DWORD64 a; DWORD b; } displacement;
        IMAGEHLP_LINE line;
        bool got_line = SymGetLineFromAddr(proc, addr, &displacement.b, &line);
        if(SymFromAddr(proc, addr, &displacement.a, symbol)) {
            if(got_line) {
                IMAGEHLP_STACK_FRAME frame;
                frame.InstructionOffset = symbol->Address;
                // https://docs.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symsetcontext
                // "If you call SymSetContext to set the context to its current value, the
                // function fails but GetLastError returns ERROR_SUCCESS."
                // This is the stupidest fucking api I've ever worked with.
                if(SymSetContext(proc, &frame, nullptr) == FALSE && GetLastError() != ERROR_SUCCESS) {
                    log::error("Stack trace: Internal error while calling SymSetContext");
                    return {
                        addr,
                        object_frame.object_address,
                        { static_cast<std::uint32_t>(line.LineNumber) },
                        nullable<std::uint32_t>::null(),
                        line.FileName,
                        symbol->Name,
                        false
                    };
                }
                DWORD n_children = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_COUNT, true>(
                    symbol->TypeIndex,
                    proc,
                    symbol->ModBase
                );
                DWORD class_parent_id = get_info<DWORD, IMAGEHLP_SYMBOL_TYPE_INFO::TI_GET_CLASSPARENTID, true>(
                    symbol->TypeIndex,
                    proc,
                    symbol->ModBase
                );
                function_info fi {
                    proc,
                    symbol->ModBase,
                    0,
                    int(n_children),
                    class_parent_id != (DWORD)-1,
                    ""
                };
                SymEnumSymbols(proc, 0, nullptr, enumerator_callback, &fi);
                std::string signature = symbol->Name + std::string("(") + fi.str + ")";
                // There's a phenomina with DIA not inserting commas after template parameters. Fix them here.
                static std::regex comma_re(R"(,(?=\S))");
                signature = std::regex_replace(signature, comma_re, ", ");
                return {
                    addr,
                    object_frame.object_address,
                    { static_cast<std::uint32_t>(line.LineNumber) },
                    nullable<std::uint32_t>::null(),
                    line.FileName,
                    signature,
                    false,
                };
            } else {
                return {
                    addr,
                    object_frame.object_address,
                    nullable<std::uint32_t>::null(),
                    nullable<std::uint32_t>::null(),
                    "",
                    symbol->Name,
                    false
                };
            }
        } else {
            return {
                addr,
                object_frame.object_address,
                nullable<std::uint32_t>::null(),
                nullable<std::uint32_t>::null(),
                "",
                "",
                false
            };
        }
    }

    std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames) {
        // Dbghelp is is single-threaded, so acquire a lock.
        auto lock = get_dbghelp_lock();
        std::vector<stacktrace_frame> trace;
        trace.reserve(frames.size());

        // TODO: When does this need to be called? Can it be moved to the symbolizer?
        SymSetOptions(SYMOPT_ALLOW_ABSOLUTE_SYMBOLS);

        auto syminit_info = ensure_syminit();
        for(const auto frame : frames) {
            try {
                trace.push_back(resolve_frame(syminit_info.get_process_handle() , frame));
            } catch(...) { // NOSONAR
                detail::log_and_maybe_propagate_exception(std::current_exception());
                auto entry = null_frame();
                entry.raw_address = frame;
                trace.push_back(entry);
            }
        }
        return trace;
    }
}
}
CPPTRACE_END_NAMESPACE

CPPTRACE_BEGIN_NAMESPACE
    /*
    When a module was loaded at runtime with LoadLibrary after SymInitialize was already called,
    it is necessary to manually load the symbols from that module with SymLoadModuleEx.

    See "Symbol Handler Initialization" in Microsoft documentation at
    https://learn.microsoft.com/en-us/windows/win32/debug/symbol-handler-initialization
    */
    void load_symbols_for_file(const std::string& filename) {
        HMODULE hModule = GetModuleHandleA(filename.c_str());
        if (hModule == NULL) {
            throw detail::internal_error(
                "Unable to get module handle for file '{}' : {}",
                filename,
                std::system_error(GetLastError(), std::system_category()).what()
            );
        }

        // SymLoadModuleEx needs the module's base address and size, so get these with GetModuleInformation.
        MODULEINFO module_info;
        if (
            !GetModuleInformation(
                GetCurrentProcess(),
                hModule,
                &module_info,
                sizeof(module_info)
            )
        ) {
            throw detail::internal_error(
                "Unable to get module information for file '{}' : {}",
                filename,
                std::system_error(GetLastError(), std::system_category()).what()
            );
        }

        auto lock = detail::get_dbghelp_lock();
        HANDLE syminit_handle = detail::ensure_syminit().get_process_handle();
        if (
            !SymLoadModuleEx(
                syminit_handle,
                NULL,
                filename.c_str(),
                NULL,
                (DWORD64)module_info.lpBaseOfDll,
                // The documentation says this is optional, but if omitted (0), symbol loading fails
                module_info.SizeOfImage,
                NULL,
                0
            )
        ) {
            throw detail::internal_error(
                "Unable to load symbols for file '{}' : {}",
                filename,
                std::system_error(GetLastError(), std::system_category()).what()
            );
        }
    }
CPPTRACE_END_NAMESPACE

#endif
