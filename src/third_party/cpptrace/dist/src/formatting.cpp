#include <cpptrace/formatting.hpp>
#include <cpptrace/utils.hpp>

#include "utils/optional.hpp"
#include "utils/utils.hpp"
#include "utils/replace_all.hpp"
#include "snippets/snippet.hpp"

#include <cstdio>
#include <string>
#include <functional>
#include <iostream>
#include <sstream>
#include <regex>

CPPTRACE_BEGIN_NAMESPACE
    std::string basename(const std::string& path) {
        return detail::basename(path, true);
    }

    std::string prettify_symbol(std::string symbol) {
        // > > -> >> replacement
        // could put in analysis:: but the replacement is basic and this is more convenient for
        // using in the stringifier too
        detail::replace_all_dynamic(symbol, "> >", ">>");
        // "," -> ", " and " ," -> ", "
        static const std::regex comma_re(R"(\s*,\s*)");
        detail::replace_all(symbol, comma_re, ", ");
        // class C -> C for msvc
        static const std::regex class_re(R"(\b(class|struct)\s+)");
        detail::replace_all(symbol, class_re, "");
        // `anonymous namespace' -> (anonymous namespace) for msvc
        // this brings it in-line with other compilers and prevents any tokenization/highlighting issues
        static const std::regex msvc_anonymous_namespace("`anonymous namespace'");
        detail::replace_all(symbol, msvc_anonymous_namespace, "(anonymous namespace)");
        // rules to replace std::basic_string -> std::string and std::basic_string_view -> std::string
        // rule to replace ", std::allocator<whatever>"
        static const std::pair<std::regex, std::string> basic_string = {
            std::regex(R"(std(::[a-zA-Z0-9_]+)?::basic_string<char)"), "std::string"
        };
        detail::replace_all_template(symbol, basic_string);
        static const std::pair<std::regex, std::string> basic_string_view = {
            std::regex(R"(std(::[a-zA-Z0-9_]+)?::basic_string_view<char)"), "std::string_view"
        };
        detail::replace_all_template(symbol, basic_string_view);
        static const std::pair<std::regex, std::string> allocator = {
            std::regex(R"(,\s*std(::[a-zA-Z0-9_]+)?::allocator<)"), ""
        };
        detail::replace_all_template(symbol, allocator);
        static const std::pair<std::regex, std::string> default_delete = {
            std::regex(R"(,\s*std(::[a-zA-Z0-9_]+)?::default_delete<)"), ""
        };
        detail::replace_all_template(symbol, default_delete);
        // replace std::__cxx11 -> std:: for gcc dual abi
        // https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_dual_abi.html
        detail::replace_all_dynamic(symbol, "std::__cxx11::", "std::");
        return symbol;
    }

    class formatter::impl {
        struct {
            std::string header = "Stack trace (most recent call first):";
            color_mode color = color_mode::automatic;
            address_mode addresses = address_mode::raw;
            path_mode paths = path_mode::full;
            bool snippets = false;
            bool break_before_filename = false;
            int context_lines = 2;
            bool columns = true;
            symbol_mode symbols = symbol_mode::full;
            bool show_filtered_frames = true;
            std::function<bool(const stacktrace_frame&)> filter;
            std::function<stacktrace_frame(stacktrace_frame)> transform;
        } options;

    public:
        void header(std::string header) {
            options.header = std::move(header);
        }
        void colors(formatter::color_mode mode) {
            options.color = mode;
        }
        void addresses(formatter::address_mode mode) {
            options.addresses = mode;
        }
        void paths(path_mode mode) {
            options.paths = mode;
        }
        void snippets(bool snippets) {
            options.snippets = snippets;
        }
        void snippet_context(int lines) {
            options.context_lines = lines;
        }
        void columns(bool columns) {
            options.columns = columns;
        }
        void symbols(symbol_mode mode) {
            options.symbols = mode;
        }
        void filtered_frame_placeholders(bool show) {
            options.show_filtered_frames = show;
        }
        void filter(std::function<bool(const stacktrace_frame&)> filter) {
            options.filter = filter;
        }
        void transform(std::function<stacktrace_frame(stacktrace_frame)> transform) {
            options.transform = std::move(transform);
        }
        void break_before_filename(bool do_break) {
            options.break_before_filename = do_break;
        }

        std::string format(
            const stacktrace_frame& frame,
            detail::optional<bool> color_override = detail::nullopt,
            size_t filename_indent = 0
        ) const {
            std::ostringstream oss;
            print_internal(oss, frame, color_override.value_or(options.color == color_mode::always), filename_indent);
            return std::move(oss).str();
        }

        std::string format(const stacktrace& trace, detail::optional<bool> color_override = detail::nullopt) const {
            std::ostringstream oss;
            print_internal(oss, trace, color_override.value_or(options.color == color_mode::always));
            return std::move(oss).str();
        }

        void print(const stacktrace_frame& frame, detail::optional<bool> color_override = detail::nullopt) const {
            print(std::cout, frame, color_override);
        }
        void print(
            std::ostream& stream,
            const stacktrace_frame& frame,
            detail::optional<bool> color_override = detail::nullopt,
            size_t filename_indent = 0
        ) const {
            print_internal(stream, frame, color_override, filename_indent);
            stream << "\n";
        }
        void print(
            std::FILE* file,
            const stacktrace_frame& frame,
            detail::optional<bool> color_override = detail::nullopt,
            size_t filename_indent = 0
        ) const {
            auto str = format(frame, color_override, filename_indent);
            str += "\n";
            std::fwrite(str.data(), 1, str.size(), file);
        }

        void print(const stacktrace& trace, detail::optional<bool> color_override = detail::nullopt) const {
            print(std::cout, trace, color_override);
        }
        void print(
            std::ostream& stream,
            const stacktrace& trace,
            detail::optional<bool> color_override = detail::nullopt
        ) const {
            print_internal(stream, trace, color_override);
            stream << "\n";
        }
        void print(
            std::FILE* file,
            const stacktrace& trace,
            detail::optional<bool> color_override = detail::nullopt
        ) const {
            auto str = format(trace, color_override);
            str += "\n";
            std::fwrite(str.data(), 1, str.size(), file);
        }

    private:
        struct color_setting {
            bool color;
            color_setting(bool color) : color(color) {}
            detail::string_view reset() const {
                return color ? RESET : "";
            }
            detail::string_view green() const {
                return color ? GREEN : "";
            }
            detail::string_view yellow() const {
                return color ? YELLOW : "";
            }
            detail::string_view blue() const {
                return color ? BLUE : "";
            }
        };

        bool stream_is_tty(std::ostream& stream) const {
            // not great, but it'll have to do
            return (&stream == &std::cout && isatty(stdout_fileno))
                || (&stream == &std::cerr && isatty(stderr_fileno));
        }

        void maybe_ensure_virtual_terminal_processing(std::ostream& stream, bool color) const {
            if(color && stream_is_tty(stream)) {
                detail::enable_virtual_terminal_processing_if_needed();
            }
        }

        bool should_do_color(std::ostream& stream, detail::optional<bool> color_override) const {
            bool do_color = options.color == color_mode::always || color_override.value_or(false);
            if(
                (options.color == color_mode::automatic || options.color == color_mode::always) &&
                (!color_override || color_override.unwrap() != false) &&
                stream_is_tty(stream)
            ) {
                do_color = true;
            }
            return do_color;
        }

        void print_internal(std::ostream& stream, const stacktrace_frame& input_frame, detail::optional<bool> color_override, size_t col_indent) const {
            bool color = should_do_color(stream, color_override);
            maybe_ensure_virtual_terminal_processing(stream, color);
            detail::optional<stacktrace_frame> transformed_frame;
            if(options.transform) {
                transformed_frame = options.transform(input_frame);
            }
            const stacktrace_frame& frame = options.transform ? transformed_frame.unwrap() : input_frame;
            write_frame(stream, frame, color, col_indent);
        }

        void print_internal(std::ostream& stream, const stacktrace& trace, detail::optional<bool> color_override) const {
            bool color = should_do_color(stream, color_override);
            maybe_ensure_virtual_terminal_processing(stream, color);
            write_trace(stream, trace, color);
        }


        void write_trace(std::ostream& stream, const stacktrace& trace, bool color) const {
            if(!options.header.empty()) {
                stream << options.header << '\n';
            }
            std::size_t counter = 0;
            const auto& frames = trace.frames;
            if(frames.empty()) {
                stream << "<empty trace>";
                return;
            }
            const auto frame_number_width = detail::n_digits(static_cast<int>(frames.size()) - 1);
            for(size_t i = 0; i < frames.size(); ++i) {
                detail::optional<stacktrace_frame> transformed_frame;
                if(options.transform) {
                    transformed_frame = options.transform(frames[i]);
                }
                const stacktrace_frame& frame = options.transform ? transformed_frame.unwrap() : frames[i];
                bool filter_out_frame = options.filter && !options.filter(frame);
                if(filter_out_frame && !options.show_filtered_frames) {
                    counter++;
                    continue;
                }

                size_t filename_indent = write_frame_number(stream, frame_number_width, counter);
                if(filter_out_frame) {
                    microfmt::print(stream, "(filtered)");
                } else {
                    write_frame(stream, frame, color, filename_indent);
                    if(frame.line.has_value() && !frame.filename.empty() && options.snippets) {
                        auto snippet = detail::get_snippet(
                            frame.filename,
                            frame.line.value(),
                            frame.column,
                            options.context_lines,
                            color
                        );
                        if(!snippet.empty()) {
                            stream << '\n';
                            stream << snippet;
                        }
                    }
                }
                if(i + 1 != frames.size()) {
                    stream << '\n';
                }
                counter++;
            }
        }

        /// Write the frame number, and return the number of characters written
        size_t write_frame_number(std::ostream& stream, unsigned int frame_number_width, size_t counter) const
        {
            microfmt::print(stream, "#{<{}} ", frame_number_width, counter);
            return 2 + frame_number_width;
        }

        void write_frame(std::ostream& stream, const stacktrace_frame& frame, color_setting color, size_t col) const {
            col += write_address(stream, frame, color);
            if(frame.is_inline || options.addresses != address_mode::none) {
                stream << ' ';
                col += 1;
            }
            if(!frame.symbol.empty()) {
                write_symbol(stream, frame, color);
            }
            if(!frame.symbol.empty() && !frame.filename.empty()) {
                if(options.break_before_filename) {
                    microfmt::print(stream, "\n{<{}}", col, "");
                } else {
                    stream << ' ';
                }
            }
            if(!frame.filename.empty()) {
                write_source_location(stream, frame, color);
            }
        }

        /// Write the address of the frame, return the number of characters written
        size_t write_address(std::ostream& stream, const stacktrace_frame& frame, color_setting color) const {
            if(frame.is_inline) {
                microfmt::print(stream, "{<{}}", 2 * sizeof(frame_ptr) + 2, "(inlined)");
                return 2 * sizeof(frame_ptr) + 2;
            } else if(options.addresses != address_mode::none) {
                auto address = options.addresses == address_mode::raw ? frame.raw_address : frame.object_address;
                microfmt::print(stream, "{}0x{>{}:0h}{}", color.blue(), 2 * sizeof(frame_ptr), address, color.reset());
                return 2 * sizeof(frame_ptr) + 2;
            }
            return 0;
        }

        void write_symbol(std::ostream& stream, const stacktrace_frame& frame, color_setting color) const {
            detail::optional<std::string> maybe_stored_string;
            detail::string_view symbol;
            switch(options.symbols) {
                case symbol_mode::full:
                    symbol = frame.symbol;
                    break;
                case symbol_mode::pruned:
                    maybe_stored_string = prune_symbol(frame.symbol);
                    symbol = maybe_stored_string.unwrap();
                    break;
                case symbol_mode::pretty:
                    maybe_stored_string = prettify_symbol(frame.symbol);
                    symbol = maybe_stored_string.unwrap();
                    break;
                default:
                    PANIC("Unhandled symbol mode");
            }
            microfmt::print(stream, "in {}{}{}", color.yellow(), symbol, color.reset());
        }

        void write_source_location(std::ostream& stream, const stacktrace_frame& frame, color_setting color) const {
            microfmt::print(
                stream,
                "at {}{}{}",
                color.green(),
                options.paths == path_mode::full ? frame.filename : detail::basename(frame.filename, true),
                color.reset()
            );
            if(frame.line.has_value()) {
                microfmt::print(stream, ":{}{}{}", color.blue(), frame.line.value(), color.reset());
                if(frame.column.has_value() && options.columns) {
                    microfmt::print(stream, ":{}{}{}", color.blue(), frame.column.value(), color.reset());
                }
            }
        }
    };

    formatter::formatter() : pimpl(new impl) {}
    formatter::~formatter() {
        delete pimpl;
    }

    formatter::formatter(formatter&& other) : pimpl(detail::exchange(other.pimpl, nullptr)) {}
    formatter::formatter(const formatter& other) : pimpl(new impl(*other.pimpl)) {}
    formatter& formatter::operator=(formatter&& other) {
        if(pimpl) {
            delete pimpl;
        }
        pimpl = detail::exchange(other.pimpl, nullptr);
        return *this;
    }
    formatter& formatter::operator=(const formatter& other) {
        if(pimpl) {
            delete pimpl;
        }
        pimpl = new impl(*other.pimpl);
        return *this;
    }

    formatter& formatter::header(std::string header) {
        pimpl->header(std::move(header));
        return *this;
    }
    formatter& formatter::colors(color_mode mode) {
        pimpl->colors(mode);
        return *this;
    }
    formatter& formatter::addresses(address_mode mode) {
        pimpl->addresses(mode);
        return *this;
    }
    formatter& formatter::paths(path_mode mode) {
        pimpl->paths(mode);
        return *this;
    }
    formatter& formatter::snippets(bool snippets) {
        pimpl->snippets(snippets);
        return *this;
    }
    formatter& formatter::snippet_context(int lines) {
        pimpl->snippet_context(lines);
        return *this;
    }
    formatter& formatter::columns(bool columns) {
        pimpl->columns(columns);
        return *this;
    }
    formatter& formatter::symbols(symbol_mode mode) {
        pimpl->symbols(mode);
        return *this;
    }
    formatter& formatter::filtered_frame_placeholders(bool show) {
        pimpl->filtered_frame_placeholders(show);
        return *this;
    }
    formatter& formatter::filter(std::function<bool(const stacktrace_frame&)> filter) {
        pimpl->filter(std::move(filter));
        return *this;
    }
    formatter& formatter::transform(std::function<stacktrace_frame(stacktrace_frame)> transform) {
        pimpl->transform(std::move(transform));
        return *this;
    }
    formatter& formatter::break_before_filename(bool do_break) {
        pimpl->break_before_filename(do_break);
        return *this;
    }

    std::string formatter::format(const stacktrace_frame& frame) const {
        return pimpl->format(frame);
    }
    std::string formatter::format(const stacktrace_frame& frame, bool color) const {
        return pimpl->format(frame, color);
    }
    std::string formatter::format(const stacktrace_frame& frame, bool color, size_t filename_indent) const {
        return pimpl->format(frame, color, filename_indent);
    }

    std::string formatter::format(const stacktrace& trace) const {
        return pimpl->format(trace);
    }
    std::string formatter::format(const stacktrace& trace, bool color) const {
        return pimpl->format(trace, color);
    }

    void formatter::print(const stacktrace& trace) const {
        pimpl->print(trace);
    }
    void formatter::print(const stacktrace& trace, bool color) const {
        pimpl->print(trace, color);
    }
    void formatter::print(std::ostream& stream, const stacktrace& trace) const {
        pimpl->print(stream, trace);
    }
    void formatter::print(std::ostream& stream, const stacktrace& trace, bool color) const {
        pimpl->print(stream, trace, color);
    }
    void formatter::print(std::FILE* file, const stacktrace& trace) const {
        pimpl->print(file, trace);
    }
    void formatter::print(std::FILE* file, const stacktrace& trace, bool color) const {
        pimpl->print(file, trace, color);
    }

    void formatter::print(const stacktrace_frame& frame) const {
        pimpl->print(frame);
    }
    void formatter::print(const stacktrace_frame& frame, bool color) const {
        pimpl->print(frame, color);
    }
    void formatter::print(std::ostream& stream, const stacktrace_frame& frame) const {
        pimpl->print(stream, frame);
    }
    void formatter::print(std::ostream& stream, const stacktrace_frame& frame, bool color) const {
        pimpl->print(stream, frame, color);
    }
    void formatter::print(std::ostream& stream, const stacktrace_frame& frame, bool color, size_t filename_indent) const {
        pimpl->print(stream, frame, color, filename_indent);
    }
    void formatter::print(std::FILE* file, const stacktrace_frame& frame) const {
        pimpl->print(file, frame);
    }
    void formatter::print(std::FILE* file, const stacktrace_frame& frame, bool color) const {
        pimpl->print(file, frame, color);
    }
    void formatter::print(std::FILE* file, const stacktrace_frame& frame, bool color, size_t filename_indent) const {
        pimpl->print(file, frame, color, filename_indent);
    }

    const formatter& get_default_formatter() {
        static formatter formatter;
        return formatter;
    }
CPPTRACE_END_NAMESPACE
