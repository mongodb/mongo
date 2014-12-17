var AmpersandState = require('ampersand-state'),
    Sidebar = require('./sidebar'),
    Chart = require('./chart'),
    _ = require('lodash'),
    debug = require('debug')('model:app');

var App = module.exports = AmpersandState.extend({
  children: {
    sidebar: Sidebar,
    chart: Chart
  },
  parse: function (attrs, options) {

    // parse series data to extract panel titles and stat names
    var panels = _.chain(attrs.series)
      // get keys
      .map(function (serie) { return serie.key; })
      // split at : and group by first token
      .groupBy(function (key) { return key.split(':')[0]; })
      // create panel documents with group title
      .map(function (value, key) { 
        // split at : and use second token as stat name
        var stats = _.map(value, function (stat) {
          return { name: stat.split(':')[1].trim() };
        });
        return { title: key, stats: stats }; 
      })
      .value();

    var ret = {
      // provide information that sidebar needs
      sidebar: {
        panels: panels,
      },
      // pass attrs to chart to do its own parsing
      chart: attrs
    };

    return ret;
  }
});

