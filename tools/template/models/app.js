var AmpersandState = require('ampersand-state'),
    Chart = require('./chart'),
    StatCollection = require('./stat-collection'),
    Sidebar = require('./sidebar'),
    _ = require('lodash'),
    debug = require('debug')('model:app');


var App = module.exports = AmpersandState.extend({
  props: {
    xSetting: {
      type: 'string',
      default: 'relative',
      values: ['relative', 'absolute']
    },
    ySetting: {
      type: 'string',
      default: 'linear',
      values: ['linear', 'log-scale']
    }
  },
  children: {
    sidebar: Sidebar,
    chart: Chart
  },
  collections: {
    stats: StatCollection
  },
  parse: function (attrs, options) {
    var groups = {};
    var stats = _.map(attrs.series, function (serie) {
      var tokens = serie.key.split(':');
      var group = tokens[0].trim();
      var name = tokens[1].trim();
      var data = _.map(serie.values, function (v, k) {
        return {x: parseInt(k), y: v};
      });

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

