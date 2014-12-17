#ifndef BOOST_THREAD_THREAD_COMMON_HPP
#define BOOST_THREAD_THREAD_COMMON_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007-10 Anthony Williams

#include <boost/thread/exceptions.hpp>
#ifndef BOOST_NO_IOSTREAM
#include <ostream>
#endif
#include <boost/thread/detail/move.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/thread/detail/thread_heap_alloc.hpp>
#include <boost/utility.hpp>
#include <boost/assert.hpp>
#include <list>
#include <algorithm>
#include <boost/ref.hpp>
#include <boost/cstdint.hpp>
#include <boost/bind.hpp>
#include <stdlib.h>
#include <memory>
#include <boost/utility/enable_if.hpp>
#include <boost/type_traits/remove_reference.hpp>
#include <boost/io/ios_state.hpp>

#include <boost/config/abi_prefix.hpp>

#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4251)
#endif

namespace boost
{
    namespace detail
    {
        template<typename F>
        class thread_data:
            public detail::thread_data_base
        {
        public:
#ifndef BOOST_NO_RVALUE_REFERENCES
            thread_data(F&& f_):
                f(static_cast<F&&>(f_))
            {}
            thread_data(F& f_):
                f(f_)
            {}
#else
            thread_data(F f_):
                f(f_)
            {}
            thread_data(detail::thread_move_t<F> f_):
                f(f_)
            {}
#endif
            void run()
            {
                f();
            }
        private:
            F f;

            void operator=(thread_data&);
            thread_data(thread_data&);
        };

        template<typename F>
        class thread_data<boost::reference_wrapper<F> >:
            public detail::thread_data_base
        {
        private:
            F& f;

            void operator=(thread_data&);
            thread_data(thread_data&);
        public:
            thread_data(boost::reference_wrapper<F> f_):
                f(f_)
            {}

            void run()
            {
                f();
            }
        };

        template<typename F>
        class thread_data<const boost::reference_wrapper<F> >:
            public detail::thread_data_base
        {
        private:
            F& f;
            void operator=(thread_data&);
            thread_data(thread_data&);
        public:
            thread_data(const boost::reference_wrapper<F> f_):
                f(f_)
            {}

            void run()
            {
                f();
            }
        };
    }

    class BOOST_THREAD_DECL thread
    {
    private:
        thread(thread&);
        thread& operator=(thread&);

        void release_handle();

        detail::thread_data_ptr thread_info;

        void start_thread();

        explicit thread(detail::thread_data_ptr data);

        detail::thread_data_ptr get_thread_info BOOST_PREVENT_MACRO_SUBSTITUTION () const;

#ifndef BOOST_NO_RVALUE_REFERENCES
        template<typename F>
        static inline detail::thread_data_ptr make_thread_info(F&& f)
        {
            return detail::thread_data_ptr(detail::heap_new<detail::thread_data<typename boost::remove_reference<F>::type> >(static_cast<F&&>(f)));
        }
        static inline detail::thread_data_ptr make_thread_info(void (*f)())
        {
            return detail::thread_data_ptr(detail::heap_new<detail::thread_data<void(*)()> >(static_cast<void(*&&)()>(f)));
        }
#else
        template<typename F>
        static inline detail::thread_data_ptr make_thread_info(F f)
        {
            return detail::thread_data_ptr(detail::heap_new<detail::thread_data<F> >(f));
        }
        template<typename F>
        static inline detail::thread_data_ptr make_thread_info(boost::detail::thread_move_t<F> f)
        {
            return detail::thread_data_ptr(detail::heap_new<detail::thread_data<F> >(f));
        }

#endif
        struct dummy;
    public:
#if BOOST_WORKAROUND(__SUNPRO_CC, < 0x5100)
        thread(const volatile thread&);
#endif
        thread();
        ~thread();

#ifndef BOOST_NO_RVALUE_REFERENCES
#ifdef BOOST_MSVC
        template <class F>
        explicit thread(F f,typename disable_if<boost::is_convertible<F&,detail::thread_move_t<F> >, dummy* >::type=0):
            thread_info(make_thread_info(static_cast<F&&>(f)))
        {
            start_thread();
        }
#else
        template <class F>
        thread(F&& f):
            thread_info(make_thread_info(static_cast<F&&>(f)))
        {
            start_thread();
        }
#endif

        thread(thread&& other)
        {
            thread_info.swap(other.thread_info);
        }

        thread& operator=(thread&& other)
        {
            thread_info=other.thread_info;
            other.thread_info.reset();
            return *this;
        }

        thread&& move()
        {
            return static_cast<thread&&>(*this);
        }

#else
#ifdef BOOST_NO_SFINAE
        template <class F>
        explicit thread(F f):
            thread_info(make_thread_info(f))
        {
            start_thread();
        }
#else
        template <class F>
        explicit thread(F f,typename disable_if<boost::is_convertible<F&,detail::thread_move_t<F> >, dummy* >::type=0):
            thread_info(make_thread_info(f))
        {
            start_thread();
        }
#endif

        template <class F>
        explicit thread(detail::thread_move_t<F> f):
            thread_info(make_thread_info(f))
        {
            start_thread();
        }

        thread(detail::thread_move_t<thread> x)
        {
            thread_info=x->thread_info;
            x->thread_info.reset();
        }

#if BOOST_WORKAROUND(__SUNPRO_CC, < 0x5100)
        thread& operator=(thread x)
        {
            swap(x);
            return *this;
        }
#else
        thread& operator=(detail::thread_move_t<thread> x)
        {
            thread new_thread(x);
            swap(new_thread);
            return *this;
        }
#endif
        operator detail::thread_move_t<thread>()
        {
            return move();
        }

        detail::thread_move_t<thread> move()
        {
            detail::thread_move_t<thread> x(*this);
            return x;
        }

#endif

        template <class F,class A1>
        thread(F f,A1 a1):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1)))
        {
            start_thread();
        }
        template <class F,class A1,class A2>
        thread(F f,A1 a1,A2 a2):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2)))
        {
            start_thread();
        }

        template <class F,class A1,class A2,class A3>
        thread(F f,A1 a1,A2 a2,A3 a3):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2,a3)))
        {
            start_thread();
        }

        template <class F,class A1,class A2,class A3,class A4>
        thread(F f,A1 a1,A2 a2,A3 a3,A4 a4):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2,a3,a4)))
        {
            start_thread();
        }

        template <class F,class A1,class A2,class A3,class A4,class A5>
        thread(F f,A1 a1,A2 a2,A3 a3,A4 a4,A5 a5):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2,a3,a4,a5)))
        {
            start_thread();
        }

        template <class F,class A1,class A2,class A3,class A4,class A5,class A6>
        thread(F f,A1 a1,A2 a2,A3 a3,A4 a4,A5 a5,A6 a6):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2,a3,a4,a5,a6)))
        {
            start_thread();
        }

        template <class F,class A1,class A2,class A3,class A4,class A5,class A6,class A7>
        thread(F f,A1 a1,A2 a2,A3 a3,A4 a4,A5 a5,A6 a6,A7 a7):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2,a3,a4,a5,a6,a7)))
        {
            start_thread();
        }

        template <class F,class A1,class A2,class A3,class A4,class A5,class A6,class A7,class A8>
        thread(F f,A1 a1,A2 a2,A3 a3,A4 a4,A5 a5,A6 a6,A7 a7,A8 a8):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2,a3,a4,a5,a6,a7,a8)))
        {
            start_thread();
        }

        template <class F,class A1,class A2,class A3,class A4,class A5,class A6,class A7,class A8,class A9>
        thread(F f,A1 a1,A2 a2,A3 a3,A4 a4,A5 a5,A6 a6,A7 a7,A8 a8,A9 a9):
            thread_info(make_thread_info(boost::bind(boost::type<void>(),f,a1,a2,a3,a4,a5,a6,a7,a8,a9)))
        {
            start_thread();
        }

        void swap(thread& x)
        {
            thread_info.swap(x.thread_info);
        }

        class BOOST_SYMBOL_VISIBLE id;
        id get_id() const;


        bool joinable() const;
        void join();
        bool timed_join(const system_time& wait_until);

        template<typename TimeDuration>
        inline bool timed_join(TimeDuration const& rel_time)
        {
            return timed_join(get_system_time()+rel_time);
        }
        void detach();

        static unsigned hardware_concurrency();

        typedef detail::thread_data_base::native_handle_type native_handle_type;
        native_handle_type native_handle();

        // backwards compatibility
        bool operator==(const thread& other) const;
        bool operator!=(const thread& other) const;

        static inline void yield()
        {
            this_thread::yield();
        }

        static inline void sleep(const system_time& xt)
        {
            this_thread::sleep(xt);
        }

        // extensions
        void interrupt();
        bool interruption_requested() const;
    };

    inline void swap(thread& lhs,thread& rhs)
    {
        return lhs.swap(rhs);
    }

#ifndef BOOST_NO_RVALUE_REFERENCES
    inline thread&& move(thread& t)
    {
        return static_cast<thread&&>(t);
    }
    inline thread&& move(thread&& t)
    {
        return static_cast<thread&&>(t);
    }
#else
    inline detail::thread_move_t<thread> move(detail::thread_move_t<thread> t)
    {
        return t;
    }
#endif

    namespace this_thread
    {
        thread::id BOOST_THREAD_DECL get_id();

        void BOOST_THREAD_DECL interruption_point();
        bool BOOST_THREAD_DECL interruption_enabled();
        bool BOOST_THREAD_DECL interruption_requested();

        inline BOOST_SYMBOL_VISIBLE void sleep(xtime const& abs_time)
        {
            sleep(system_time(abs_time));
        }
    }

    class BOOST_SYMBOL_VISIBLE thread::id
    {
    private:
        detail::thread_data_ptr thread_data;

        id(detail::thread_data_ptr thread_data_):
            thread_data(thread_data_)
        {}
        friend class thread;
        friend id BOOST_THREAD_DECL this_thread::get_id();
    public:
        id():
            thread_data()
        {}

        id(const id& other):
            thread_data(other.thread_data)
        {}

        bool operator==(const id& y) const
        {
            return thread_data==y.thread_data;
        }

        bool operator!=(const id& y) const
        {
            return thread_data!=y.thread_data;
        }

        bool operator<(const id& y) const
        {
            return thread_data<y.thread_data;
        }

        bool operator>(const id& y) const
        {
            return y.thread_data<thread_data;
        }

        bool operator<=(const id& y) const
        {
            return !(y.thread_data<thread_data);
        }

        bool operator>=(const id& y) const
        {
            return !(thread_data<y.thread_data);
        }

#ifndef BOOST_NO_IOSTREAM
#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
        template<class charT, class traits>
        friend BOOST_SYMBOL_VISIBLE
	std::basic_ostream<charT, traits>&
        operator<<(std::basic_ostream<charT, traits>& os, const id& x)
        {
            if(x.thread_data)
            {
                io::ios_flags_saver  ifs( os );
                return os<< std::hex << x.thread_data;
            }
            else
            {
                return os<<"{Not-any-thread}";
            }
        }
#else
        template<class charT, class traits>
        BOOST_SYMBOL_VISIBLE
	std::basic_ostream<charT, traits>&
        print(std::basic_ostream<charT, traits>& os) const
        {
            if(thread_data)
            {
                return os<<thread_data;
            }
            else
            {
                return os<<"{Not-any-thread}";
            }
        }

#endif
#endif
    };

#if !defined(BOOST_NO_IOSTREAM) && defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
    template<class charT, class traits>
    BOOST_SYMBOL_VISIBLE
    std::basic_ostream<charT, traits>&
    operator<<(std::basic_ostream<charT, traits>& os, const thread::id& x)
    {
        return x.print(os);
    }
#endif

    inline bool thread::operator==(const thread& other) const
    {
        return get_id()==other.get_id();
    }

    inline bool thread::operator!=(const thread& other) const
    {
        return get_id()!=other.get_id();
    }

    namespace detail
    {
        struct thread_exit_function_base
        {
            virtual ~thread_exit_function_base()
            {}
            virtual void operator()()=0;
        };

        template<typename F>
        struct thread_exit_function:
            thread_exit_function_base
        {
            F f;

            thread_exit_function(F f_):
                f(f_)
            {}

            void operator()()
            {
                f();
            }
        };

        void BOOST_THREAD_DECL add_thread_exit_function(thread_exit_function_base*);
    }

#ifdef BOOST_NO_RVALUE_REFERENCES
    template <>
    struct has_move_emulation_enabled_aux<thread>
      : BOOST_MOVE_BOOST_NS::integral_constant<bool, true>
    {};
#endif

    namespace this_thread
    {
        template<typename F>
        void at_thread_exit(F f)
        {
            detail::thread_exit_function_base* const thread_exit_func=detail::heap_new<detail::thread_exit_function<F> >(f);
            detail::add_thread_exit_function(thread_exit_func);
        }
    }
}

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

#include <boost/config/abi_suffix.hpp>

#endif
