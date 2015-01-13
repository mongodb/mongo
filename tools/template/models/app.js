var AmpersandState = require('ampersand-state'),
    Chart = require('./chart'),
    StatCollection = require('./stat-collection'),
    Sidebar = require('./sidebar'),
    _ = require('lodash'),
    debug = require('debug')('model:app');


var App = module.exports = AmpersandState.extend({
  children: {
    sidebar: Sidebar,
    chart: Chart
  },
  collections: {
    stats: StatCollection
  },
  props: {
    selectionState: {
      type: 'array',
      default: function () { return []; }
    }
  },
  parse: function (attrs, options) {
    var year = new Date().getFullYear();
    var groups = {};
    var stats = _.map(attrs.series, function (serie) {
      if (serie.key.indexOf(':') !== -1) {
        var tokens = serie.key.split(':');
        var group = tokens[0].trim();
        var name = tokens[1].trim();
      } else {
        var group = 'stats';
        var name = serie.key;
      }

      var data = _.sortBy(
        _.map(serie.values, function (v, k) {
          // add current year to date string
          var tokens = k.split(' ');
          tokens.splice(2, 0, year.toString());
          var d = tokens.join(' ');
          return {x: new Date(d), y: v};
        }), 'x');
      // calculate relative x values per series
      var minx = Math.min.apply(null, data.map(function (d) { return d.x }));
      data.forEach(function (d) {
        d.xrel = (d.x - minx) / 1000;
      })
      groups[group] = true;
      return { group: group, name: name, data: data };
    });

    groups = _.keys(groups).map(function (title) {
      return {title: title};
    });

    var ret = {
      stats: stats,
      // provide information that sidebar needs
      sidebar: {
        panels: groups,
      }
    };
    return ret;
  },
  initialize: function (attrs, options) {
    // tell the panels about app and what stats they're managing
    var panels = this.sidebar.panels;
    panels.each(function (panel) {
      panel.app = this;
    }.bind(this));
    
    this.stats.each(function (stat) {
      stat.app = this;
      panels.get(stat.group).stats.add(stat);
    }.bind(this));
  },
  clearSelectionState: function () {
    this.selectionState = [];
    debug('clear');
  },
  toggleAllExcept: function (stat) {
    // shift-click on stat has the following behavior:
    // if any other stat is selected, then 
    //     1. store current selection state
    //     2. enable current stat
    //     3. disable all other stats
    // else
    //     restore previous selection state

    var deselectAll = this.stats
      .filter(function (s) { return s !== stat })
      .some(function (s) { return s.selected; });

    if (this.selectionState.length === 0 || !stat.selected) {
      // store old selected state
      this.selectionState = this.stats.map(function (s) { return s.selected; });

      // now deselect all but stat
      this.stats.each(function (s) {
        s.selected = (s === stat);
      });

    } else {
      // need to restore previous selection state
      this.stats.each(function (s, i) {
        s.selected = this.selectionState.length ? this.selectionState[i] : true;
      }.bind(this));
      this.clearSelectionState();
    }
  }
});

