/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <cairomm/context.h>
#include <gtkmm.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "io_trace.h"
#include "plot.h"
#include "util.h"

/* The minimum distance in pixels for a mouse drag to be recognized as such. */
#define BTE_PLOT_MIN_DRAG_DISTANCE 10

/*
 * plot_widget::plot_widget --
 *     Initialize a new plot_widget object.
 */
plot_widget::plot_widget(plot_group &group, const io_trace &trace)
    : _group(group), _trace(trace), _margin_top(20), _margin_bottom(30), _margin_left(120),
      _margin_right(10)
{
    _group.add(*this);
    _plot_tool = plot_tool::NONE;
    set_size_request(300, _margin_top);

    /* Set up event handlers. */

    set_draw_func(sigc::mem_fun(*this, &plot_widget::on_draw));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    gtk_widget_add_controller(((Gtk::Widget *)this)->gobj(), GTK_EVENT_CONTROLLER(drag));
    g_signal_connect(drag, "drag-begin", G_CALLBACK(plot_widget::static_drag_begin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(plot_widget::static_drag_update), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(plot_widget::static_drag_end), this);

    /* Get the min and max data coordinates to initialize the view. */

    const std::vector<io_trace_operation> &ops = _trace.operations();

    _toplevel_view.min_x = ops[0].timestamp;
    _toplevel_view.max_x = ops[ops.size() - 1].timestamp;

    double dx = abs(_toplevel_view.max_x - _toplevel_view.min_x);
    if (dx < 1e-12) {
        _toplevel_view.max_x += 0.5;
        _toplevel_view.min_x -= 0.5;
        if (_toplevel_view.min_x < 0)
            _toplevel_view.min_x = 0;
    }

    unsigned long minOffset = LONG_MAX;
    unsigned long maxOffset = 0;

    for (const auto &t : ops) {
        if (t.offset < minOffset)
            minOffset = t.offset;
        if (t.offset + t.length > maxOffset)
            maxOffset = t.offset + t.length;
    }

    _toplevel_view.min_y = minOffset;
    _toplevel_view.max_y = maxOffset;

    double dy = abs(_toplevel_view.max_y - _toplevel_view.min_y);
    if (dy < 1e-12) {
        _toplevel_view.max_y += 0.5;
        _toplevel_view.min_y -= 0.5;
        if (_toplevel_view.min_y < 0)
            _toplevel_view.min_y = 0;
    }

    _view = _toplevel_view;
}

/*
 * plot_widget::~plot_widget --
 *     Destroy the plot.
 */
plot_widget::~plot_widget() {}

/*
 * plot_widget::on_drag_begin --
 *     The event handler for mouse drag begin.
 */
void
plot_widget::on_drag_begin(GtkGestureDrag *, double x, double y)
{
    int pixbuf_width = get_width() - _margin_left - _margin_right;
    int pixbuf_height = get_height() - _margin_top - _margin_bottom;

    x = std::max(std::min(x - _margin_left, (double)pixbuf_width), 0.0);
    y = std::max(std::min(y - _margin_top, (double)pixbuf_height), 0.0);

    _drag = true;
    _drag_horizontal = false;
    _drag_vertical = false;

    _drag_start_x = (int)x;
    _drag_start_y = (int)y;
    _drag_end_x = _drag_last_x = _drag_start_x;
    _drag_end_y = _drag_last_y = _drag_start_y;

    queue_draw();
}

/*
 * plot_widget::on_drag_update --
 *     The event handler for mouse drag update.
 */
void
plot_widget::on_drag_update(GtkGestureDrag *, double x, double y)
{
    int pixbuf_width = get_width() - _margin_left - _margin_right;
    int pixbuf_height = get_height() - _margin_top - _margin_bottom;

    _drag_end_x = _drag_start_x + (int)x;
    _drag_end_y = _drag_start_y + (int)y;
    _drag_end_x = std::max(std::min(_drag_end_x, pixbuf_width), 0);
    _drag_end_y = std::max(std::min(_drag_end_y, pixbuf_height), 0);

    if (_plot_tool == plot_tool::MOVE) {
        double x1 = _drag_last_x;
        double x2 = _drag_end_x;
        double y1 = _drag_last_y;
        double y2 = _drag_end_y;
        double dx = (_view.max_x - _view.min_x) * (x2 - x1) / (double)pixbuf_width;
        double dy = (_view.max_y - _view.min_y) * (y2 - y1) / (double)pixbuf_height;

        plot_view view = _view;
        view.min_x -= dx;
        view.max_x -= dx;
        view.min_y += dy;
        view.max_y += dy;
        set_view(view, true);
    }

    double dx = std::abs(_drag_start_x - _drag_end_x);
    double dy = std::abs(_drag_start_y - _drag_end_y);
    if (dx >= BTE_PLOT_MIN_DRAG_DISTANCE) {
        if (!_drag_vertical || (dx / dy > 0.15))
            _drag_horizontal = true;
    }
    if (dy >= BTE_PLOT_MIN_DRAG_DISTANCE) {
        if (!_drag_horizontal || (dy / dx > 0.15))
            _drag_vertical = true;
    }

    _drag_last_x = _drag_end_x;
    _drag_last_y = _drag_end_y;

    queue_draw();
}

/*
 * plot_widget::on_drag_end --
 *     The event handler for mouse drag end.
 */
void
plot_widget::on_drag_end(GtkGestureDrag *gesture, double x, double y)
{
    int pixbuf_width = get_width() - _margin_left - _margin_right;
    int pixbuf_height = get_height() - _margin_top - _margin_bottom;

    _drag = false;
    _drag_end_x = _drag_start_x + (int)x;
    _drag_end_y = _drag_start_y + (int)y;
    _drag_end_x = std::max(std::min(_drag_end_x, pixbuf_width), 0);
    _drag_end_y = std::max(std::min(_drag_end_y, pixbuf_height), 0);

    double x1 = _drag_start_x;
    double x2 = _drag_end_x;
    double y1 = _drag_start_y;
    double y2 = _drag_end_y;

    if (x1 > x2)
        std::swap(x1, x2);
    if (y1 < y2)
        std::swap(y1, y2);

    if (_plot_tool == plot_tool::ZOOM) {
        if (_drag_horizontal || _drag_vertical) {
            plot_view view = _view;
            if (_drag_horizontal) {
                view.min_x = _view.view_to_data_x(x1, pixbuf_width);
                view.max_x = _view.view_to_data_x(x2, pixbuf_width);
            }
            if (_drag_vertical) {
                view.min_y = _view.view_to_data_y(y1, pixbuf_height);
                view.max_y = _view.view_to_data_y(y2, pixbuf_height);
            }

            set_view(view);
        }
    }

    _drag_last_x = _drag_end_x;
    _drag_last_y = _drag_end_y;

    queue_draw();
}

/*
 * plot_widget::static_drag_begin --
 *     The event handler for mouse drag begin - the static version for the C bindings.
 */
void
plot_widget::static_drag_begin(GtkGestureDrag *gesture, double x, double y, plot_widget *widget)
{
    widget->on_drag_begin(gesture, x, y);
}

/*
 * plot_widget::static_drag_update --
 *     The event handler for mouse drag update - the static version for the C bindings.
 */
void
plot_widget::static_drag_update(GtkGestureDrag *gesture, double x, double y, plot_widget *widget)
{
    widget->on_drag_update(gesture, x, y);
}

/*
 * plot_widget::static_drag_end --
 *     The event handler for mouse drag end - the static version for the C bindings.
 */
void
plot_widget::static_drag_end(GtkGestureDrag *gesture, double x, double y, plot_widget *widget)
{
    widget->on_drag_end(gesture, x, y);
}

/*
 * plot_widget::view_back --
 *     Go back to the previous view (undo the view change).
 */
void
plot_widget::view_back()
{
    if (_view_undo.empty())
        return;

    _view_redo.push_back(_view);
    _view = _view_undo.back();
    _view_undo.pop_back();

    queue_draw();
}

/*
 * plot_widget::view_forward --
 *     Go forward to the view that we had just before the last view undo (redo the view change).
 */
void
plot_widget::view_forward()
{
    if (_view_redo.empty())
        return;

    _view_undo.push_back(_view);
    _view = _view_redo.back();
    _view_redo.pop_back();

    queue_draw();
}

/*
 * plot_widget::view_reset --
 *     Reset the view.
 */
void
plot_widget::view_reset()
{
    _view_undo.push_back(_view);
    _view_redo.clear();
    _view = _toplevel_view;
    queue_draw();
}

/*
 * plot_widget::zoom_in --
 *     Zoom in.
 */
void
plot_widget::zoom_in()
{
    plot_view view = _view;
    double z = 0.1;
    double dx = view.max_x - view.min_x;
    double dy = view.max_y - view.min_y;
    view.min_x += dx * z;
    view.max_x -= dx * z;
    view.min_y += dy * z;
    view.max_y -= dy * z;
    set_view(view);
    queue_draw();
}

/*
 * plot_widget::zoom_out --
 *     Zoom out.
 */
void
plot_widget::zoom_out()
{
    plot_view view = _view;
    double z = 0.1;
    double dx = view.max_x - view.min_x;
    double dy = view.max_y - view.min_y;
    view.min_x -= dx / (1 - 2 * z) * z;
    view.max_x += dx / (1 - 2 * z) * z;
    view.min_y -= dy / (1 - 2 * z) * z;
    view.max_y += dy / (1 - 2 * z) * z;
    set_view(view);
    queue_draw();
}

/*
 * plot_widget::set_view --
 *     Set the viewport. This can be optionally done "in place," i.e., without modifying the
 *  undo/redo stacks.
 */
void
plot_widget::set_view(const plot_view &view, bool in_place)
{
    if (!in_place) {
        _view_undo.push_back(_view);
        _view_redo.clear();
    }

    _view = view;
    _group.view_sync(*this, in_place);
}

/*
 * plot_widget::view_sync --
 *     Synchronize view from the given source view.
 */
void
plot_widget::view_sync(const plot_view &source, bool in_place)
{
    if (!in_place) {
        _view_undo.push_back(_view);
        _view_redo.clear();
    }

    plot_view view = _view;
    view.min_x = source.min_x;
    view.max_x = source.max_x;
    _view = view;

    queue_draw();
}

/*
 * plot_widget::render_worker --
 *     Render a part of the plot that is between the start and the end indexes.
 */
int
plot_widget::render_worker(const std::vector<io_trace_operation> &trace, int start, int end)
{
    int count = 0;
    int pixbuf_n_channels = _pixbuf->get_n_channels();
    int pixbuf_rowstride = _pixbuf->get_rowstride();
    int pixbuf_width = _pixbuf->get_width();
    int pixbuf_height = _pixbuf->get_height();
    guchar *pixels = _pixbuf->get_pixels();
    size_t capacity = pixbuf_height * pixbuf_rowstride;

    guchar *drawn = (guchar *)malloc(capacity);
    memset(drawn, 0, capacity);

    for (int trace_index = start; trace_index < end; trace_index++) {
        const io_trace_operation &t = trace[trace_index];

        int x1 = _view.data_to_view_x(t.timestamp, pixbuf_width);
        int y1 = _view.data_to_view_y(t.offset, pixbuf_height);
        int x2 = _view.data_to_view_x(t.timestamp + std::min(0.00001, t.duration), pixbuf_width);
        int y2 = _view.data_to_view_y(t.offset + t.length, pixbuf_height);

        if (x2 < 0 || x1 >= pixbuf_width)
            continue;
        if (y1 < 0 || y2 >= pixbuf_width)
            continue;

        x1 = std::max(x1, 0);
        x2 = std::min(x2, pixbuf_width - 1);
        y2 = std::max(y2, 0);
        y1 = std::min(y1, pixbuf_height - 1);

        if (x2 < x1 || y1 < y2)
            continue;

        count++;

        int p = y2 * pixbuf_rowstride + x1 * pixbuf_n_channels;
        assert(p <= capacity);
        if (drawn[p])
            continue;
        drawn[p] = 1;

        unsigned color = t.read ? 0x60c060 : 0x800000;
        for (int y = y2; y <= y1; y++) {
            p = y * pixbuf_rowstride + x1 * pixbuf_n_channels;
            for (int x = x1; x <= x2; x++) {
                unsigned *pointer = (unsigned *)(void *)&pixels[p];
                *pointer = (*pointer & 0xff000000u) | color;
                p += 3;
            }
            assert(p <= capacity);
        }
    }

    free(drawn);
    return count;
}

/*
 * get_axis_unit --
 *     Find a good distance between two tickmarks.
 */
static double
get_axis_unit(int view_range, double data_range, bool bytes = false)
{
    if (data_range < 1e-12)
        return 1;

    double start_unit = 1;
    const int SCALE_NUMS_BYTES[] = {1, 2, 4, 8};
    const int SCALE_NUMS_REG[] = {1, 2, 5, 10};
    const int SCALE_NUMS_COUNT = 4;
    const int *SCALE_NUMS = bytes ? SCALE_NUMS_BYTES : SCALE_NUMS_REG;

    while (true) {
        int i = 0;
        int last = 0;
        double unit = start_unit;

        if (unit * view_range / data_range > 600) {
            start_unit /= bytes ? 8 : 10;
            continue;
        }

        while (unit * view_range / data_range < 100) {
            i = (i + 1) % SCALE_NUMS_COUNT;
            unit = unit * SCALE_NUMS[i];
            if (i > 0)
                unit /= SCALE_NUMS[last];
            last = i;
        }

        return unit;
    }
}

/*
 * plot_widget::on_draw --
 *     Render the plot.
 */
void
plot_widget::on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int width, int height)
{
    Cairo::TextExtents extents;

    if (width <= 0 || height <= 0)
        return;

    /* Clear the drawing area. */

    cr->save();
    cr->set_source_rgba(1, 1, 1, 1);
    cr->paint();
    cr->restore();

    /* Title. */

    cr->save();
    cr->get_text_extents(_trace.name(), extents);
    cr->move_to(4, 4 + extents.height);
    cr->show_text(_trace.name());
    cr->restore();

    /* Render the data. */

    const std::vector<io_trace_operation> &trace = _trace.operations();

    int pixbuf_width = width - _margin_left - _margin_right;
    int pixbuf_height = height - _margin_top - _margin_bottom;
    if (pixbuf_width <= 0 || pixbuf_height <= 0 || trace.empty())
        return;

    double start_time = current_time();
    std::atomic<long> count_data(0);
    if (_pixbuf.get() == nullptr || _pixbuf->get_width() != pixbuf_width ||
      _pixbuf->get_height() != height || _view != _pixbuf_view) {
        _pixbuf_view = _view;
        _pixbuf = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, pixbuf_width, pixbuf_height);
        guchar *pixels = _pixbuf->get_pixels();
        memset(pixels, 0xff, pixbuf_height * _pixbuf->get_rowstride());

        int min_data_x = _view.view_to_data_x(0, pixbuf_width);
        int max_data_x = _view.view_to_data_x(pixbuf_width, pixbuf_width);
        long min_data_i = std::lower_bound(trace.begin(), trace.end(),
                            io_trace_operation::wrap_timestamp(min_data_x * 0.95)) -
          trace.begin();
        long max_data_i = std::upper_bound(trace.begin(), trace.end(),
                            io_trace_operation::wrap_timestamp(max_data_x * 1.05)) -
          trace.begin();
        long data_length = max_data_i - min_data_i;

        int num_threads = data_length < 10000 ? 1 : 8;
        std::vector<std::thread> workers;
        for (int i = 0; i < num_threads; i++) {
            int start = min_data_i + i * data_length / num_threads;
            int end = min_data_i + (i + 1) * data_length / num_threads;
            workers.push_back(std::thread([this, &trace, &count_data, start, end]() {
                count_data += render_worker(trace, start, end);
            }));
        }
        std::for_each(workers.begin(), workers.end(), [](std::thread &t) { t.join(); });
    }
    double render_time = current_time() - start_time;
    if (render_time > 0.5) {
        g_warning("Rendering the plot took %.2lf seconds (%.2lf mil. data points)", render_time,
          count_data / 1.0e6);
    }

    cr->save();
    Gdk::Cairo::set_source_pixbuf(cr, _pixbuf, _margin_left, _margin_top);
    cr->paint();
    cr->restore();

    /* Draw the inspection cross-hairs. */

    if (_drag && _plot_tool == plot_tool::INSPECT) {
        cr->save();
        cr->set_source_rgba(0.5, 0.5, 0.5, 0.7);

        double x = _margin_left + _drag_end_x;
        double y = _margin_top + _drag_end_y;

        cr->move_to(_margin_left, y);
        cr->line_to(_margin_left + pixbuf_width, y);
        cr->move_to(x, _margin_top);
        cr->line_to(x, _margin_top + pixbuf_height);

        cr->stroke();
        cr->restore();
    }

    /* Draw the rectangle for mouse drag. */

    if (_drag && _plot_tool == plot_tool::ZOOM) {
        cr->save();
        cr->set_source_rgba(0.0, 0.0, 1.0, 0.7);

        double x1 = _margin_left + _drag_start_x;
        double x2 = _margin_left + _drag_end_x;
        double y1 = _margin_top + _drag_start_y;
        double y2 = _margin_top + _drag_end_y;
        if (x1 > x2)
            std::swap(x1, x2);
        if (y1 < y2)
            std::swap(y1, y2);

        if (_drag_horizontal && !_drag_vertical) {
            y1 = _margin_top;
            y2 = _margin_top + pixbuf_height;
        } else if (!_drag_horizontal && _drag_vertical) {
            x1 = _margin_left;
            x2 = _margin_left + pixbuf_width;
        }

        cr->rectangle(x1, y1, x2 - x1, y2 - y1);
        cr->fill();
        cr->restore();
    }

    /* Draw the plot axes. */

    cr->save();
    cr->set_source_rgba(0, 0, 0, 1);
    cr->move_to(_margin_left, _margin_top + pixbuf_height);
    cr->line_to(_margin_left + pixbuf_width, _margin_top + pixbuf_height);
    cr->move_to(_margin_left, _margin_top + pixbuf_height);
    cr->line_to(_margin_left, _margin_top);
    cr->stroke();
    cr->restore();

    double scale_y = 1048576.0;
    double unit_x = get_axis_unit(pixbuf_width, _view.max_x - _view.min_x);
    double unit_y = get_axis_unit(pixbuf_height, (_view.max_y - _view.min_y) / scale_y, true);

    cr->save();
    double m = std::floor(_toplevel_view.min_x / unit_x) * unit_x;
    while (_view.data_to_view_x(m, pixbuf_width) < pixbuf_width) {
        int p = _view.data_to_view_x(m, pixbuf_width);
        if (p >= 0) {
            cr->move_to(_margin_left + p, _margin_top + pixbuf_height);
            cr->line_to(_margin_left + p, _margin_top + pixbuf_height + 4);
            cr->stroke();

            char text[64];
            snprintf(text, sizeof(text), "%.*lf", (int)-std::min(floor(log10(unit_x)), 0.0), m);

            cr->get_text_extents(text, extents);
            cr->move_to(_margin_left + p - extents.width / 2,
              _margin_top + pixbuf_height + 8 + extents.height / 2);
            cr->show_text(text);
        }
        m += unit_x;
    }
    cr->restore();

    cr->save();
    m = std::floor(_toplevel_view.min_y / unit_y / scale_y) * unit_y;
    while (_view.data_to_view_y(m * scale_y, pixbuf_height) > 0) {
        int p = _view.data_to_view_y(m * scale_y, pixbuf_height);
        if (p < pixbuf_height) {
            cr->move_to(_margin_left, _margin_top + p);
            cr->line_to(_margin_left - 4, _margin_top + p);
            cr->stroke();

            char text[64];
            char str_k[32];
            if ((long)m >= 1000)
                snprintf(text, sizeof(text), "%ld,%03ldM", ((long)m) / 1000, ((long)m) % 1000);
            else
                snprintf(text, sizeof(text), "%.0lfM", m);
            if (unit_y < 0.999) {
                snprintf(str_k, sizeof(str_k), " + %dK", (int)round(m * 1024) % 1024);
                strcat(text, str_k);
            }

            cr->get_text_extents(text, extents);
            cr->move_to(_margin_left - 8 - extents.width, _margin_top + p + extents.height / 2);
            cr->show_text(text);
        }
        m += unit_y;
    }
    cr->restore();
}

/*
 * plot_group::plot_group --
 *     Create a new plot group.
 */
plot_group::plot_group() : _active_plot(NULL) {}

/*
 * plot_group::~plot_group --
 *     Destroy the plot group.
 */
plot_group::~plot_group() {}

/*
 * plot_group::add --
 *     Add another plot to the group.
 */
void
plot_group::add(plot_widget &plot)
{
    _plots.push_back(&plot);
    if (_active_plot == NULL)
        _active_plot = &plot;
}

/*
 * plot_group::view_back --
 *     Go back to the previous views (undo the view change).
 */
void
plot_group::view_back()
{
    for (plot_widget *p : _plots)
        p->view_back();
}

/*
 * plot_group::view_forward --
 *     Go forward to the views that we had just before the last view undo (redo the view change).
 */
void
plot_group::view_forward()
{
    for (plot_widget *p : _plots)
        p->view_forward();
}

/*
 * plot_group::view_reset --
 *     Reset the view.
 */
void
plot_group::view_reset()
{
    for (plot_widget *p : _plots)
        p->view_reset();
}

/*
 * plot_group::view_reset_x --
 *     Reset just the X axes across all plots.
 */
void
plot_group::view_reset_x()
{
    double min_x = INFINITY;
    double max_x = -INFINITY;
    for (plot_widget *p : _plots) {
        min_x = std::min(min_x, p->_view.min_x);
        max_x = std::max(max_x, p->_view.max_x);
    }
    for (plot_widget *p : _plots) {
        p->_toplevel_view.min_x = p->_view.min_x = min_x;
        p->_toplevel_view.max_x = p->_view.max_x = max_x;
    }
}

/*
 * plot_group::view_sync --
 *     Synchronize the views from the given source.
 */
void
plot_group::view_sync(plot_widget &source, bool in_place)
{
    _active_plot = &source;
    for (plot_widget *p : _plots) {
        if (&source != p)
            p->view_sync(source._view, in_place);
    }
}
