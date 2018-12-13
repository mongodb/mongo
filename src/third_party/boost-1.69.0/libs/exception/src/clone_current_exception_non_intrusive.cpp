//Copyright (c) 2006-2009 Emil Dotchevski and Reverge Studios, Inc.

//Distributed under the Boost Software License, Version 1.0. (See accompanying
//file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//This MSVC-specific cpp file implements non-intrusive cloning of exception objects.
//Based on an exception_ptr implementation by Anthony Williams.

#ifdef BOOST_NO_EXCEPTIONS
#error This file requires exception handling to be enabled.
#endif

#include <boost/config.hpp>
#include <boost/exception/detail/clone_current_exception.hpp>

#if defined(BOOST_ENABLE_NON_INTRUSIVE_EXCEPTION_PTR) && defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))

//Non-intrusive cloning support implemented below, only for MSVC versions mentioned above.
//Thanks Anthony Williams!
//Thanks to Martin Weiss for implementing 64-bit support!

#include <boost/exception/exception.hpp>
#include <boost/shared_ptr.hpp>
#include <windows.h>
#include <malloc.h>

namespace
    {
    unsigned const exception_maximum_parameters=15;
    unsigned const exception_noncontinuable=1;

#if _MSC_VER==1310
    int const exception_info_offset=0x74;
#elif ((_MSC_VER==1400 || _MSC_VER==1500) && !defined _M_X64)
    int const exception_info_offset=0x80;
#elif ((_MSC_VER==1400 || _MSC_VER==1500) && defined _M_X64)
    int const exception_info_offset=0xE0;
#else
    int const exception_info_offset=-1;
#endif

    struct
    exception_record
        {
        unsigned long ExceptionCode;
        unsigned long ExceptionFlags;
        exception_record * ExceptionRecord;
        void * ExceptionAddress;
        unsigned long NumberParameters;
        ULONG_PTR ExceptionInformation[exception_maximum_parameters];
        };

    struct
    exception_pointers
        {
        exception_record * ExceptionRecord;
        void * ContextRecord;
        };

    unsigned const cpp_exception_code=0xE06D7363;
    unsigned const cpp_exception_magic_flag=0x19930520;
#ifdef _M_X64
    unsigned const cpp_exception_parameter_count=4;
#else
    unsigned const cpp_exception_parameter_count=3;
#endif

    struct
    dummy_exception_type
        {
        };

    typedef int(dummy_exception_type::*normal_copy_constructor_ptr)(void * src);
    typedef int(dummy_exception_type::*copy_constructor_with_virtual_base_ptr)(void * src,void * dst);
    typedef void (dummy_exception_type::*destructor_ptr)();

    union
    cpp_copy_constructor
        {
        void * address;
        normal_copy_constructor_ptr normal_copy_constructor;
        copy_constructor_with_virtual_base_ptr copy_constructor_with_virtual_base;
        };

    union
    cpp_destructor
        {
        void * address;
        destructor_ptr destructor;
        };

    enum
    cpp_type_flags
        {
        class_is_simple_type=1,
        class_has_virtual_base=4
        };

    // ATTENTION: On x86 fields such as type_info and copy_constructor are really pointers
    // but on 64bit these are 32bit offsets from HINSTANCE. Hints on the 64bit handling from
    // http://blogs.msdn.com/b/oldnewthing/archive/2010/07/30/10044061.aspx .
    struct
    cpp_type_info
        {
        unsigned flags;
        int type_info;
        int this_offset;
        int vbase_descr;
        int vbase_offset;
        unsigned long size;
        int copy_constructor;
        };

    struct
    cpp_type_info_table
        {
        unsigned count;
        int info;
        };

    struct
    cpp_exception_type
        {
        unsigned flags;
        int destructor;
        int custom_handler;
        int type_info_table;
        };

    struct
    exception_object_deleter
        {
        cpp_exception_type const & et_;
        size_t image_base_;

        exception_object_deleter( cpp_exception_type const & et, size_t image_base ):
            et_(et),
            image_base_(image_base)
            {
            }

        void
        operator()( void * obj )
            {
            BOOST_ASSERT(obj!=0);
            dummy_exception_type* dummy_exception_ptr = static_cast<dummy_exception_type *>(obj);
            if( et_.destructor )
                {
                cpp_destructor destructor;
                destructor.address = reinterpret_cast<void *>(et_.destructor + image_base_);
                (dummy_exception_ptr->*(destructor.destructor))();
                }
            free(obj);
            }
        };

    cpp_type_info const &
    get_cpp_type_info( cpp_exception_type const & et, size_t image_base )
        {
        cpp_type_info_table * const typearray = reinterpret_cast<cpp_type_info_table * const>(et.type_info_table + image_base);
        cpp_type_info * const ti = reinterpret_cast<cpp_type_info * const>(typearray->info + image_base);
        BOOST_ASSERT(ti!=0);
        return *ti;
        }

    void
    copy_msvc_exception( void * dst, void * src, cpp_type_info const & ti, size_t image_base )
        {
        cpp_copy_constructor copy_constructor;
        copy_constructor.address = reinterpret_cast<void *>(ti.copy_constructor + image_base);

        if( !(ti.flags & class_is_simple_type) && copy_constructor.normal_copy_constructor )
            {
            dummy_exception_type * dummy_exception_ptr = static_cast<dummy_exception_type *>(dst);
            if( ti.flags & class_has_virtual_base )
                (dummy_exception_ptr->*(copy_constructor.copy_constructor_with_virtual_base))(src,dst);
            else
                (dummy_exception_ptr->*(copy_constructor.normal_copy_constructor))(src);
            }
        else
            memmove(dst,src,ti.size);
        }

    boost::shared_ptr<void>
    clone_msvc_exception( void * src, cpp_exception_type const & et, size_t image_base )
        {
        BOOST_ASSERT(src!=0);
        cpp_type_info const & ti=get_cpp_type_info(et,image_base);
        if( void * dst = malloc(ti.size) )
            {
            try
                {
                copy_msvc_exception(dst,src,ti,image_base);
                }
            catch(
            ... )
                {
                free(dst);
                throw;
                }
            return boost::shared_ptr<void>(dst,exception_object_deleter(et,image_base));
            }
        else
            throw std::bad_alloc();
        }

    class
    cloned_exception:
        public boost::exception_detail::clone_base
        {
        cloned_exception( cloned_exception const & );
        cloned_exception & operator=( cloned_exception const & );

        cpp_exception_type const & et_;
        size_t image_base_;
        boost::shared_ptr<void> exc_;

        public:
        cloned_exception( EXCEPTION_RECORD const * record ):
            et_(*reinterpret_cast<cpp_exception_type const *>(record->ExceptionInformation[2])),
            image_base_((cpp_exception_parameter_count==4) ? record->ExceptionInformation[3] : 0),
            exc_(clone_msvc_exception(reinterpret_cast<void *>(record->ExceptionInformation[1]),et_,image_base_))
            {
            }

        cloned_exception( void * exc, cpp_exception_type const & et, size_t image_base ):
            et_(et),
            image_base_(image_base),
            exc_(clone_msvc_exception(exc,et_,image_base))
            {
            }

        ~cloned_exception() throw()
            {
            }

        boost::exception_detail::clone_base const *
        clone() const
            {
            return new cloned_exception(exc_.get(),et_,image_base_);
            }

        void
        rethrow() const
            {
            cpp_type_info const & ti=get_cpp_type_info(et_,image_base_);
            void * dst = _alloca(ti.size);
            copy_msvc_exception(dst,exc_.get(),ti,image_base_);
            ULONG_PTR args[cpp_exception_parameter_count];
            args[0]=cpp_exception_magic_flag;
            args[1]=reinterpret_cast<ULONG_PTR>(dst);
            args[2]=reinterpret_cast<ULONG_PTR>(&et_);
            if (cpp_exception_parameter_count==4)
                args[3]=image_base_;

            RaiseException(cpp_exception_code,EXCEPTION_NONCONTINUABLE,cpp_exception_parameter_count,args);
            }
        };

    bool
    is_cpp_exception( EXCEPTION_RECORD const * record )
        {
        return record && 
            (record->ExceptionCode==cpp_exception_code) &&
            (record->NumberParameters==cpp_exception_parameter_count) &&
            (record->ExceptionInformation[0]==cpp_exception_magic_flag);
        }

    unsigned long
    exception_cloning_filter( int & result, boost::exception_detail::clone_base const * & ptr, void * info_ )
        {
        BOOST_ASSERT(exception_info_offset>=0);
        BOOST_ASSERT(info_!=0);
        EXCEPTION_RECORD* record = static_cast<EXCEPTION_POINTERS *>(info_)->ExceptionRecord;
        if( is_cpp_exception(record) )
            {
            if( !record->ExceptionInformation[2] )
                record = *reinterpret_cast<EXCEPTION_RECORD * *>(reinterpret_cast<char *>(_errno())+exception_info_offset);
            if( is_cpp_exception(record) && record->ExceptionInformation[2] )
                try
                    {
                    ptr = new cloned_exception(record);
                    result = boost::exception_detail::clone_current_exception_result::success;
                    }
                catch(
                std::bad_alloc & )
                    {
                    result = boost::exception_detail::clone_current_exception_result::bad_alloc;
                    }
                catch(
                ... )
                    {
                    result = boost::exception_detail::clone_current_exception_result::bad_exception;
                    }
            }
        return EXCEPTION_EXECUTE_HANDLER;
        }
    }

namespace
boost
    {
    namespace
    exception_detail
        {
        int
        clone_current_exception_non_intrusive( clone_base const * & cloned )
            {
            BOOST_ASSERT(!cloned);
            int result = clone_current_exception_result::not_supported;
            if( exception_info_offset>=0 )
                {
                 clone_base const * ptr=0;
                __try
                    {
                    throw;
                    }
                __except(exception_cloning_filter(result,ptr,GetExceptionInformation()))
                    {
                    }
                if( result==clone_current_exception_result::success )
                    cloned=ptr;
                }
            BOOST_ASSERT(result!=clone_current_exception_result::success || cloned);
            return result;
            }
        }
    }

#else

//On all other compilers, return clone_current_exception_result::not_supported.
//On such platforms, only the intrusive enable_current_exception() cloning will work.

namespace
boost
    {
    namespace
    exception_detail
        {
        int
        clone_current_exception_non_intrusive( clone_base const * & )
            {
            return clone_current_exception_result::not_supported;
            }
        }
    }

#endif
