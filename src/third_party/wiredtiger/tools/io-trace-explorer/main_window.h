/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <gtkmm.h>

#include "io_trace.h"
#include "plot.h"

/*
 * main_window --
 *     The main window.
 */
class main_window : public Gtk::ApplicationWindow {

public:
    main_window(io_trace_collection &traces);
    ~main_window();

protected:
    /* The toolbar (in the order of appearance). */
    Gtk::Box _toolbar_box;
    Gtk::Button _back_button;
    Gtk::Button _forward_button;
    Gtk::Label _label1;
    Gtk::ToggleButton _inspect_toggle;
    Gtk::ToggleButton _move_toggle;
    Gtk::ToggleButton _zoom_toggle;
    Gtk::Label _label2;
    Gtk::Button _zoom_in_button;
    Gtk::Button _zoom_out_button;
    Gtk::Button _reset_view_button;

    /* Event connections for the tool toggle buttons. */
    sigc::connection _inspect_toggle_connection;
    sigc::connection _move_toggle_connection;
    sigc::connection _zoom_toggle_connection;

    /* The main area. */
    Gtk::Box _main_box;
    plot_group _plot_group;
    std::vector<plot_widget *> _plots;
    std::vector<Gtk::Paned *> _panes;

    /* The status bar. */
    Gtk::Statusbar _status_bar;

    /* Set the tool for user interaction with the plots. */
    void set_plot_tool(plot_tool tool);

    /* Event handlers. */
    void on_inspect_toggle();
    void on_move_toggle();
    void on_zoom_toggle();
    void on_view_back();
    void on_view_forward();
    void on_view_reset();
    void on_zoom_in();
    void on_zoom_out();
    bool on_window_key_pressed(guint key, guint, Gdk::ModifierType state);
};
