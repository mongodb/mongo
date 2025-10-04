/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <glibmm/main.h>
#include <gtkmm.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>

#include "io_trace.h"
#include "main_window.h"
#include "plot.h"
#include "util.h"

/*
 * io_trace_explorer --
 *     The application object.
 */
class io_trace_explorer : public Gtk::Application {

public:
    /*
     * io_trace_explorer --
     *     Create the application object.
     */
    io_trace_explorer()
        : Gtk::Application(
            "com.mongodb.block-trace-explorer", Gio::Application::Flags::HANDLES_COMMAND_LINE)
    {
    }

protected:
    /*
     * on_command_line --
     *     Parse the command-line arguments.
     */
    int
    on_command_line(const Glib::RefPtr<Gio::ApplicationCommandLine> &cmd)
    {

        Glib::OptionContext ctx("INPUT_FILE...");
        Glib::OptionGroup group("options", "Progran Options:", "Show the main program options");

        /* The --quiet argument. */
        bool quiet = false;
        Glib::OptionEntry quiet_entry;
        quiet_entry.set_long_name("quiet");
        quiet_entry.set_description("Suppress unnecessary output");
        group.add_entry(quiet_entry, quiet);
        ctx.add_group(group);

        try {

            /* Parse the command-line arguments. */
            int argc;
            char **argv = cmd->get_arguments(argc);
            ctx.parse(argc, argv);

            std::vector<std::string> input_files;
            for (int i = optind; i < argc; i++)
                input_files.push_back(std::string(argv[i]));

            if (input_files.empty())
                throw std::runtime_error("No input files.");

            /* Load the input files. */
            double start_time = current_time();
            for (auto s : input_files) {
                if (!quiet)
                    g_message("Loading %s", s.c_str());
                _traces.load_from_file(s);
            }
            double load_time = current_time() - start_time;
            if (!quiet)
                g_message("Loaded the data in %.2lf seconds.", load_time);

            /* Start the application. */
            activate();
            return (0);

        } catch (std::exception &e) {
            g_error("%s", e.what());
            return (EXIT_FAILURE);
        }
    }

    /*
     * on_activate --
     *     Start (activate) the application.
     */
    void
    on_activate()
    {
        _main = new main_window(_traces);
        add_window(*_main);
        _main->show();
        _main->grab_focus();
    }

    Gtk::ApplicationWindow *_main;
    io_trace_collection _traces;
};

/**
 * main --
 *     The main function for the application.
 */
int
main(int argc, char **argv)
{
    return io_trace_explorer().run(argc, argv);
}
