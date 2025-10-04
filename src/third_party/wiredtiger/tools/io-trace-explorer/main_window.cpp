/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "io_trace.h"
#include "main_window.h"
#include "plot.h"

/*
 * main_window::main_window --
 *     Create the main window object.
 */
main_window::main_window(io_trace_collection &traces)
    : _toolbar_box(Gtk::Orientation::HORIZONTAL), _back_button("<"), _forward_button(">"),
      _label1("     "), _inspect_toggle("_Inspect", true), _move_toggle("_Move", true),
      _zoom_toggle("_Zoom", true), _label2("     "), _zoom_in_button("+"), _zoom_out_button("-"),
      _reset_view_button("_Reset", true), _main_box(Gtk::Orientation::VERTICAL)
{
    set_title("I/O Trace Explorer");

    /* Create the plots */

    for (auto iter : traces.traces()) {
        plot_widget *p = new plot_widget(_plot_group, *(iter.second));
        p->set_expand(true);
        _plots.push_back(p);
    }
    _plot_group.view_reset_x();

    /* The toolbar */

    _toolbar_box.append(_back_button);
    _toolbar_box.append(_forward_button);
    _toolbar_box.append(_label1);
    _toolbar_box.append(_inspect_toggle);
    _toolbar_box.append(_move_toggle);
    _toolbar_box.append(_zoom_toggle);
    _toolbar_box.append(_label2);
    _toolbar_box.append(_zoom_in_button);
    _toolbar_box.append(_zoom_out_button);
    _toolbar_box.append(_reset_view_button);

    _inspect_toggle_connection = _inspect_toggle.signal_toggled().connect(
      sigc::mem_fun(*this, &main_window::on_inspect_toggle));
    _move_toggle_connection =
      _move_toggle.signal_toggled().connect(sigc::mem_fun(*this, &main_window::on_move_toggle));
    _zoom_toggle_connection =
      _zoom_toggle.signal_toggled().connect(sigc::mem_fun(*this, &main_window::on_zoom_toggle));

    _back_button.signal_clicked().connect(sigc::mem_fun(*this, &main_window::on_view_back));
    _forward_button.signal_clicked().connect(sigc::mem_fun(*this, &main_window::on_view_forward));
    _zoom_in_button.signal_clicked().connect(sigc::mem_fun(*this, &main_window::on_zoom_in));
    _zoom_out_button.signal_clicked().connect(sigc::mem_fun(*this, &main_window::on_zoom_out));
    _reset_view_button.signal_clicked().connect(sigc::mem_fun(*this, &main_window::on_view_reset));

    set_plot_tool(plot_tool::INSPECT);

    /* The main view */

    _main_box.set_expand();
    _main_box.append(_toolbar_box);

    Gtk::Paned *first_paned = NULL;
    Gtk::Paned *last_paned = NULL;
    plot_widget *last_plot = NULL;

    for (plot_widget *p : _plots) {
        if (last_paned == NULL) {
            if (last_plot != NULL) {
                /* This is the second plot: Create a new pane. */
                Gtk::Paned *paned = new Gtk::Paned(Gtk::Orientation::VERTICAL);
                paned->set_wide_handle(true);
                paned->set_expand(true);
                paned->set_start_child(*last_plot);
                _panes.push_back(paned);
                first_paned = last_paned = paned;
            }
        } else {
            /* Subsequent plots. */
            Gtk::Paned *paned = new Gtk::Paned(Gtk::Orientation::VERTICAL);
            paned->set_wide_handle(true);
            paned->set_expand(true);
            paned->set_start_child(*last_plot);
            _panes.push_back(paned);
            last_paned->set_end_child(*paned);
            last_paned = paned;
        }
        last_plot = p;
    }

    if (last_paned == NULL) {
        if (last_plot != NULL)
            _main_box.append(*last_plot);
    } else {
        last_paned->set_end_child(*last_plot);
        _main_box.append(*first_paned);
    }

    /* Set up events */

    auto keyController = Gtk::EventControllerKey::create();
    keyController->signal_key_pressed().connect(
      sigc::mem_fun(*this, &main_window::on_window_key_pressed), false);
    add_controller(keyController);

    /* Finish setting up the window */

    _main_box.append(_status_bar);
    set_child(_main_box);
    set_resizable(true);
    set_default_size(1024, 768);
}

/*
 * main_window::~main_window --
 *     Delete the main window.
 */
main_window::~main_window()
{
    /*
     * TODO: Do we need to clean up plots and panes? It seems that the destructor is not called on
     * application exit.
     */
}

/*
 * main_window::set_plot_tool --
 *     Set the tool for user interaction.
 */
void
main_window::set_plot_tool(plot_tool tool)
{
    for (plot_widget *p : _plots)
        p->set_active_tool(tool);

    _inspect_toggle_connection.block();
    _move_toggle_connection.block();
    _zoom_toggle_connection.block();

    _inspect_toggle.set_active(tool == plot_tool::INSPECT);
    _move_toggle.set_active(tool == plot_tool::MOVE);
    _zoom_toggle.set_active(tool == plot_tool::ZOOM);

    _inspect_toggle_connection.unblock();
    _move_toggle_connection.unblock();
    _zoom_toggle_connection.unblock();
}

/*
 * main_window::on_inspect_toggle --
 *     Event handler for the "inspect" tool toggle button.
 */
void
main_window::on_inspect_toggle()
{
    set_plot_tool(plot_tool::INSPECT);
}

/*
 * main_window::on_move_toggle --
 *     Event handler for the "move" tool toggle button.
 */
void
main_window::on_move_toggle()
{
    set_plot_tool(plot_tool::MOVE);
}

/*
 * main_window::on_zoom_toggle --
 *     Event handler for the "zoom" tool toggle button.
 */
void
main_window::on_zoom_toggle()
{
    set_plot_tool(plot_tool::ZOOM);
}

/*
 * main_window::on_view_back --
 *     Event handler for the view back button.
 */
void
main_window::on_view_back()
{
    _plot_group.view_back();
}

/*
 * main_window::on_view_forward --
 *     Event handler for the view forward button.
 */
void
main_window::on_view_forward()
{
    _plot_group.view_forward();
}

/*
 * main_window::on_zoom_in --
 *     Event handler for the zoom in button.
 */
void
main_window::on_zoom_in()
{
    if (_plot_group.active_plot())
        _plot_group.active_plot()->zoom_in();
}

/*
 * main_window::on_zoom_out --
 *     Event handler for the zoom out button.
 */
void
main_window::on_zoom_out()
{
    if (_plot_group.active_plot())
        _plot_group.active_plot()->zoom_out();
}

/*
 * main_window::on_view_reset --
 *     Event handler for the reset view button.
 */
void
main_window::on_view_reset()
{
    _plot_group.view_reset();
}

/*
 * main_window::on_window_key_pressed --
 *     Event handler for key press events.
 */
bool
main_window::on_window_key_pressed(guint key, guint, Gdk::ModifierType state)
{
    if (key == GDK_KEY_BackSpace ||
      (key == GDK_KEY_z &&
        (state & Gdk::ModifierType::META_MASK) == Gdk::ModifierType::META_MASK) ||
      (key == GDK_KEY_z &&
        (state & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK)) {
        on_view_back();
        return true;
    }

    if ((key == GDK_KEY_Z &&
          (state & Gdk::ModifierType::META_MASK) == Gdk::ModifierType::META_MASK) ||
      (key == GDK_KEY_Z &&
        (state & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK)) {
        on_view_forward();
        return true;
    }

    if (key == GDK_KEY_minus || key == GDK_KEY_underscore) {
        on_zoom_out();
        return true;
    }

    if (key == GDK_KEY_plus || key == GDK_KEY_equal) {
        on_zoom_in();
        return true;
    }

    if (key == GDK_KEY_i || key == GDK_KEY_I) {
        on_inspect_toggle();
        return true;
    }

    if (key == GDK_KEY_m || key == GDK_KEY_M) {
        on_move_toggle();
        return true;
    }

    if (key == GDK_KEY_z || key == GDK_KEY_Z) {
        on_zoom_toggle();
        return true;
    }

    if (key == GDK_KEY_r || key == GDK_KEY_R) {
        on_view_reset();
        return true;
    }

    return false;
}
