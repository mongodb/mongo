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
    // tell the panels what stats they're managing
    var panels = this.sidebar.panels;
    this.stats.each(function (stat) {
      panels.get(stat.group).stats.add(stat);
    });
  }
});

