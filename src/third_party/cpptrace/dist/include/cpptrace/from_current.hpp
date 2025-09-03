#ifndef CPPTRACE_FROM_CURRENT_HPP
#define CPPTRACE_FROM_CURRENT_HPP

#include <exception>
#include <typeinfo>
#include <utility>

#ifdef _MSC_VER
 #include <windows.h>
#endif

#include <cpptrace/basic.hpp>
#include <cpptrace/from_current_macros.hpp>

CPPTRACE_BEGIN_NAMESPACE
    CPPTRACE_EXPORT const raw_trace& raw_trace_from_current_exception();
    CPPTRACE_EXPORT const stacktrace& from_current_exception();

    CPPTRACE_EXPORT const raw_trace& raw_trace_from_current_exception_rethrow();
    CPPTRACE_EXPORT const stacktrace& from_current_exception_rethrow();

    CPPTRACE_EXPORT bool current_exception_was_rethrown();

    CPPTRACE_NORETURN CPPTRACE_EXPORT CPPTRACE_FORCE_NO_INLINE
    void rethrow();

    CPPTRACE_NORETURN CPPTRACE_EXPORT CPPTRACE_FORCE_NO_INLINE
    void rethrow(std::exception_ptr exception);

    CPPTRACE_EXPORT void clear_current_exception_traces();

    namespace detail {
        template<typename>
        struct argument;
        template<typename R, typename Arg>
        struct argument<R(Arg)> {
            using type = Arg;
        };
        template<typename R>
        struct argument<R(...)> {
            using type = void;
        };

        #ifdef _MSC_VER
         CPPTRACE_EXPORT CPPTRACE_FORCE_NO_INLINE
         int maybe_collect_trace(EXCEPTION_POINTERS* exception_ptrs, int filter_result);
         CPPTRACE_EXPORT CPPTRACE_FORCE_NO_INLINE
         void maybe_collect_trace(EXCEPTION_POINTERS* exception_ptrs, const std::type_info& type_info);
         template<typename E>
         CPPTRACE_FORCE_NO_INLINE inline int exception_filter(EXCEPTION_POINTERS* exception_ptrs) {
             maybe_collect_trace(exception_ptrs, typeid(E));
             return EXCEPTION_CONTINUE_SEARCH;
         }
         class dont_return_from_try_catch_macros {
         public:
             explicit dont_return_from_try_catch_macros() = default;
         };
        #else
         CPPTRACE_EXPORT CPPTRACE_FORCE_NO_INLINE
         void maybe_collect_trace(const std::type_info*, const std::type_info*, void**, unsigned);
         template<typename T>
         class unwind_interceptor {
         public:
             static int init;
             CPPTRACE_FORCE_NO_INLINE static bool can_catch(
                 const std::type_info* /* this */,
                 const std::type_info* throw_type,
                 void** throw_obj,
                 unsigned outer
             ) {
                 maybe_collect_trace(&typeid(T), throw_type, throw_obj, outer);
                 return false;
             }
         };
         CPPTRACE_EXPORT void do_prepare_unwind_interceptor(
             const std::type_info&,
             bool(*)(const std::type_info*, const std::type_info*, void**, unsigned)
         );
         template<typename T>
         inline int prepare_unwind_interceptor() {
             do_prepare_unwind_interceptor(typeid(unwind_interceptor<T>), unwind_interceptor<T>::can_catch);
             return 1;
         }
         template<typename T>
         int unwind_interceptor<T>::init = prepare_unwind_interceptor<T>();
         template<typename F>
         using unwind_interceptor_for = unwind_interceptor<typename argument<F>::type>;
         inline void nop(int) {}
        #endif
    }

    namespace detail {
        template<typename R, typename Arg>
        Arg get_callable_argument_helper(R(*) (Arg));
        template<typename R, typename F, typename Arg>
        Arg get_callable_argument_helper(R(F::*) (Arg));
        template<typename R, typename F, typename Arg>
        Arg get_callable_argument_helper(R(F::*) (Arg) const);
        template<typename R>
        void get_callable_argument_helper(R(*) ());
        template<typename R, typename F>
        void get_callable_argument_helper(R(F::*) ());
        template<typename R, typename F>
        void get_callable_argument_helper(R(F::*) () const);
        template<typename F>
        decltype(get_callable_argument_helper(&F::operator())) get_callable_argument_wrapper(F);
        template<typename T>
        using get_callable_argument = decltype(get_callable_argument_wrapper(std::declval<T>()));

        template<typename E, typename F, typename Catch, typename std::enable_if<!std::is_same<E, void>::value, int>::type = 0>
        void do_try_catch(F&& f, Catch&& catcher) {
            CPPTRACE_TRY {
                std::forward<F>(f)();
            } CPPTRACE_CATCH(E e) {
                std::forward<Catch>(catcher)(std::forward<E>(e));
            }
        }

        template<typename E, typename F, typename Catch, typename std::enable_if<std::is_same<E, void>::value, int>::type = 0>
        void do_try_catch(F&& f, Catch&& catcher) {
            CPPTRACE_TRY {
                std::forward<F>(f)();
            } CPPTRACE_CATCH(...) {
                std::forward<Catch>(catcher)();
            }
        }

        template<typename F>
        void try_catch_impl(F&& f) {
            std::forward<F>(f)();
        }

        // TODO: This could be made more efficient to reduce the number of interceptor levels that do typeid checks
        // and possible traces
        template<typename F, typename Catch, typename... Catches>
        void try_catch_impl(F&& f, Catch&& catcher, Catches&&... catches) {
            // match the first catch at the inner-most level... no real way to reverse a pack or extract from the end so
            // we have to wrap with a lambda
            auto wrapped = [&] () {
                using E = get_callable_argument<Catch>;
                do_try_catch<E>(std::forward<F>(f), std::forward<Catch>(catcher));
            };
            try_catch_impl(std::move(wrapped), std::forward<Catches>(catches)...);
        }
    }

    template<typename F, typename... Catches>
    void try_catch(F&& f, Catches&&... catches) {
        return detail::try_catch_impl(std::forward<F>(f), std::forward<Catches>(catches)...);
    }
CPPTRACE_END_NAMESPACE

#endif
