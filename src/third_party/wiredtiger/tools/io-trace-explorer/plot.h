/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <glibmm/main.h>
#include <gtkmm.h>
#include <iostream>

#include "io_trace.h"

/*
 * plot_view --
 *     A view definition for the plot; i.e., what is visible within the given viewport.
 */
struct plot_view {
    double min_x = 0;
    double max_x = 0;
    double min_y = 0;
    double max_y = 0;

    /*
     * data_to_view_x --
     *     Convert a data X value to the corresponding viewport coordinate.
     */
    inline int
    data_to_view_x(double x, int width) const
    {
        if (max_x == min_x)
            return 0;
        return (int)(width * (x - min_x) / (max_x - min_x));
    }

    /*
     * data_to_view_t --
     *     Convert a data Y value to the corresponding viewport coordinate.
     */
    inline int
    data_to_view_y(double y, int height) const
    {
        if (max_y == min_y)
            return 0;
        return (int)(height - 1 - height * (y - min_y) / (max_y - min_y));
    }

    /*
     * view_to_data_x --
     *     Convert a viewport X coordinate to the corresponding data value.
     */
    inline double
    view_to_data_x(double x, int width) const
    {
        if (width == 0)
            return min_x;
        return x * (max_x - min_x) / width + min_x;
    }

    /*
     * view_to_data_y --
     *     Convert a viewport Y coordinate to the corresponding data value.
     */
    inline double
    view_to_data_y(double y, int height) const
    {
        if (height == 0)
            return min_y;
        return (height - 1 - y) * (max_y - min_y) / height + min_y;
    }

    /*
     * operator== --
     *     The equality operator.
     */
    inline bool
    operator==(const plot_view &other) const
    {
        return min_x == other.min_x && max_x == other.max_x && min_y == other.min_y &&
          max_y == other.max_y;
    }

    /*
     * operator!= --
     *     The inequality operator.
     */
    inline bool
    operator!=(const plot_view &other) const
    {
        return min_x != other.min_x || max_x != other.max_x || min_y != other.min_y ||
          max_y != other.max_y;
    }
};

/*
 * plot_tool --
 *     A tool for user interaction with the plot.
 */
enum class plot_tool {
    NONE,
    INSPECT,
    MOVE,
    ZOOM,
};

class plot_group;

/*
 * plot_widget --
 *     The GTK+ component for drawing the plot.
 */
class plot_widget : public Gtk::DrawingArea {

    friend class plot_group;

public:
    plot_widget(plot_group &group, const io_trace &trace);
    virtual ~plot_widget();

    void view_back();
    void view_forward();
    void view_reset();
    void zoom_in();
    void zoom_out();

    /*
     * active_tool --
     *     Get the active tool.
     */
    inline plot_tool
    active_tool()
    {
        return _plot_tool;
    }

    /*
     * set_active_tool --
     *     Set the active tool.
     */
    inline void
    set_active_tool(plot_tool tool)
    {
        _plot_tool = tool;
    }

protected:
    void set_view(const plot_view &view, bool in_place = false);

    void on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int width, int height);
    void on_drag_begin(GtkGestureDrag *gesture, double x, double y);
    void on_drag_update(GtkGestureDrag *gesture, double x, double y);
    void on_drag_end(GtkGestureDrag *gesture, double x, double y);

    plot_group &_group;
    const io_trace &_trace;
    plot_tool _plot_tool;

    bool _drag;
    bool _drag_horizontal;
    bool _drag_vertical;

    int _drag_start_x;
    int _drag_start_y;
    int _drag_last_x;
    int _drag_last_y;
    int _drag_end_x;
    int _drag_end_y;

    plot_view _toplevel_view;
    plot_view _view;
    std::vector<plot_view> _view_undo;
    std::vector<plot_view> _view_redo;

    Glib::RefPtr<Gdk::Pixbuf> _pixbuf; /* The pixel buffer for the actual plot. */
    plot_view _pixbuf_view; /* The view coordinates for checking whether the plot needs to be
                               re-rendered. */

    int _margin_top;
    int _margin_bottom;
    int _margin_left;
    int _margin_right;

private:
    static void static_drag_begin(GtkGestureDrag *gesture, double x, double y, plot_widget *widget);
    static void static_drag_update(
      GtkGestureDrag *gesture, double x, double y, plot_widget *widget);
    static void static_drag_end(GtkGestureDrag *gesture, double x, double y, plot_widget *widget);

    int render_worker(const std::vector<io_trace_operation> &trace, int start, int end);
    void view_sync(const plot_view &source, bool in_place = false);
};

/*
 * plot_group --
 *     A collection of plots that should be synchronized, e.g., by having the same X axis.
 */
class plot_group {

    friend class plot_widget;

public:
    plot_group();
    virtual ~plot_group();

    void view_back();
    void view_forward();
    void view_reset();
    void view_reset_x();
    void view_sync(plot_widget &source, bool in_place = false);

    /*
     * active_plot --
     *     Get the active plot, i.e., the last plot with which the user interacted.
     */
    inline plot_widget *
    active_plot()
    {
        return _active_plot;
    }

protected:
    void add(plot_widget &plot);

    std::vector<plot_widget *> _plots;
    plot_widget *_active_plot;
};
