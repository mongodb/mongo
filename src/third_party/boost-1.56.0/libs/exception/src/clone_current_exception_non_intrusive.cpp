//Copyright (c) 2006-2009 Emil Dotchevski and Reverge Studios, Inc.

//Distributed under the Boost Software License, Version 1.0. (See accompanying
//file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//This MSVC-specific cpp file implements non-intrusive cloning of exception objects.
//Based on an exception_ptr implementation by Anthony Williams.

#ifdef BOOST_NO_EXCEPTIONS
#error This file requires exception handling to be enabled.
#endif

#include <boost/exception/detail/clone_current_exception.hpp>

#if defined(BOOST_ENABLE_NON_INTRUSIVE_EXCEPTION_PTR) && defined(_MSC_VER) && defined(_M_IX86) && !defined(_M_X64)

//Non-intrusive cloning support implemented below, only for MSVC versions mentioned above.
//Thanks Anthony Williams!

#include <boost/exception/exception.hpp>
#include <boost/shared_ptr.hpp>
#ifndef BOOST_NO_RTTI
#include <typeinfo>
#endif
#include <windows.h>
#include <malloc.h>

namespace
    {
    unsigned const exception_maximum_parameters=15;
    unsigned const exception_noncontinuable=1;

#if _MSC_VER==1310
    int const exception_info_offset=0x74;
#elif (_MSC_VER==1400 || _MSC_VER==1500)
    int const exception_info_offset=0x80;
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
    unsigned const cpp_exception_parameter_count=3;

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
        normal_copy_constructor_ptr normal_copy_constructor;
        copy_constructor_with_virtual_base_ptr copy_constructor_with_virtual_base;
        };

    enum
    cpp_type_flags
        {
        class_is_simple_type=1,
        class_has_virtual_base=4
        };

    struct
    cpp_type_info
        {
        unsigned flags;
#ifndef BOOST_NO_RTTI
        void const * type_info;
#else
        std::type_info * type_info;
#endif
        int this_offset;
        int vbase_descr;
        int vbase_offset;
        unsigned long size;
        cpp_copy_constructor copy_constructor;
        };

    struct
    cpp_type_info_table
        {
        unsigned count;
        const cpp_type_info * info[1];
        };

    struct
    cpp_exception_type
        {
        unsigned flags;
        destructor_ptr destructor;
        void(*custom_handler)();
        cpp_type_info_table const * type_info_table;
        };

    struct
    exception_object_deleter
        {
        cpp_exception_type const & et_;

        exception_object_deleter( cpp_exception_type const & et ):
            et_(et)
            {
            }

        void
        operator()( void * obj )
            {
            BOOST_ASSERT(obj!=0);
            dummy_exception_type * dummy_exception_ptr=reinterpret_cast<dummy_exception_type *>(obj);
            (dummy_exception_ptr->*(et_.destructor))();
            free(obj);
            }
        };

    cpp_type_info const &
    get_cpp_type_info( cpp_exception_type const & et )
        {
        cpp_type_info const * ti = et.type_info_table->info[0];
        BOOST_ASSERT(ti!=0);
        return *ti;
        }

    void
    copy_msvc_exception( void * dst, void * src, cpp_type_info const & ti )
        {
        if( !(ti.flags & class_is_simple_type) && ti.copy_constructor.normal_copy_constructor )
            {
            dummy_exception_type * dummy_exception_ptr = reinterpret_cast<dummy_exception_type *>(dst);
            if( ti.flags & class_has_virtual_base )
                (dummy_exception_ptr->*(ti.copy_constructor.copy_constructor_with_virtual_base))(src,dst);
            else
                (dummy_exception_ptr->*(ti.copy_constructor.normal_copy_constructor))(src);
            }
        else
            memmove(dst,src,ti.size);
        }

    boost::shared_ptr<void>
    clone_msvc_exception( void * src, cpp_exception_type const & et )
        {
        assert(src!=0);
        cpp_type_info const & ti=get_cpp_type_info(et);
        if( void * dst = malloc(ti.size) )
            {
            try
                {
                copy_msvc_exception(dst,src,ti);
                }
            catch(
            ... )
                {
                free(dst);
                throw;
                }
            return boost::shared_ptr<void>(dst,exception_object_deleter(et));
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
        boost::shared_ptr<void> exc_;

        public:

        cloned_exception( void * exc, cpp_exception_type const & et ):
            et_(et),
            exc_(clone_msvc_exception(exc,et_))
            {
            }

        ~cloned_exception() throw()
            {
            }

        boost::exception_detail::clone_base const *
        clone() const
            {
            return new cloned_exception(exc_.get(),et_);
            }

        void
        rethrow() const
            {
            cpp_type_info const & ti=get_cpp_type_info(et_);
            void * dst = _alloca(ti.size);
            copy_msvc_exception(dst,exc_.get(),ti);
            ULONG_PTR args[cpp_exception_parameter_count];
            args[0]=cpp_exception_magic_flag;
            args[1]=reinterpret_cast<ULONG_PTR>(dst);
            args[2]=reinterpret_cast<ULONG_PTR>(&et_);
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
        EXCEPTION_POINTERS * info=reinterpret_cast<EXCEPTION_POINTERS *>(info_);
        EXCEPTION_RECORD * record=info->ExceptionRecord;
        if( is_cpp_exception(record) )
            {
            if( !record->ExceptionInformation[2] )
                record = *reinterpret_cast<EXCEPTION_RECORD * *>(reinterpret_cast<char *>(_errno())+exception_info_offset);
            if( is_cpp_exception(record) && record->ExceptionInformation[2] )
                try
                    {
                    ptr = new cloned_exception(
                            reinterpret_cast<void *>(record->ExceptionInformation[1]),
                            *reinterpret_cast<cpp_exception_type const *>(record->ExceptionInformation[2]));
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

#include <boost/config.hpp>

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
