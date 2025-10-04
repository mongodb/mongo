#ifndef CPPTRACE_FORMATTING_HPP
#define CPPTRACE_FORMATTING_HPP

#include <cpptrace/basic.hpp>

#include <string>
#include <functional>

CPPTRACE_BEGIN_NAMESPACE
    class CPPTRACE_EXPORT formatter {
        class impl;
        // can't be a std::unique_ptr due to msvc awfulness with dllimport/dllexport and https://stackoverflow.com/q/4145605/15675011
        impl* pimpl;

    public:
        formatter();
        ~formatter();

        formatter(formatter&&);
        formatter(const formatter&);
        formatter& operator=(formatter&&);
        formatter& operator=(const formatter&);

        formatter& header(std::string);
        enum class color_mode {
            always,
            none,
            automatic,
        };
        formatter& colors(color_mode);
        enum class address_mode {
            raw,
            object,
            none,
        };
        formatter& addresses(address_mode);
        enum class path_mode {
            // full path is used
            full,
            // only the file name is used
            basename,
        };
        formatter& paths(path_mode);
        formatter& snippets(bool);
        formatter& snippet_context(int);
        formatter& columns(bool);
        enum class symbol_mode {
            // full demangled symbol
            full,
            // symbols are prettified to clean up some especially long template argument lists
            pretty,
            // template arguments and function parameters are pruned
            pruned
        };
        formatter& symbols(symbol_mode);
        formatter& filtered_frame_placeholders(bool);
        formatter& filter(std::function<bool(const stacktrace_frame&)>);
        formatter& transform(std::function<stacktrace_frame(stacktrace_frame)>);
        formatter& break_before_filename(bool do_break = true);

        std::string format(const stacktrace_frame&) const;
        std::string format(const stacktrace_frame&, bool color) const;
        // The last argument is the indent to use for the filename, if break_before_filename is set
        std::string format(const stacktrace_frame&, bool color, size_t filename_indent) const;

        std::string format(const stacktrace&) const;
        std::string format(const stacktrace&, bool color) const;

        void print(const stacktrace_frame&) const;
        void print(const stacktrace_frame&, bool color) const;
        void print(std::ostream&, const stacktrace_frame&) const;
        void print(std::ostream&, const stacktrace_frame&, bool color) const;
        // The last argument is the indent to use for the filename, if break_before_filename is set
        void print(std::ostream&, const stacktrace_frame&, bool color, size_t filename_indent) const;
        void print(std::FILE*, const stacktrace_frame&) const;
        void print(std::FILE*, const stacktrace_frame&, bool color) const;
        // The last argument is the indent to use for the filename, if break_before_filename is set
        void print(std::FILE*, const stacktrace_frame&, bool color, size_t filename_indent) const;

        void print(const stacktrace&) const;
        void print(const stacktrace&, bool color) const;
        void print(std::ostream&, const stacktrace&) const;
        void print(std::ostream&, const stacktrace&, bool color) const;
        void print(std::FILE*, const stacktrace&) const;
        void print(std::FILE*, const stacktrace&, bool color) const;
    };

    CPPTRACE_EXPORT const formatter& get_default_formatter();
CPPTRACE_END_NAMESPACE

#endif
