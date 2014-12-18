var AmpersandState = require('ampersand-state'),
    _ = require('lodash'),
    debug = require('debug')('model:chart');

var Chart = module.exports = AmpersandState.extend({
  derived: {
    // proxy stats from parent
    series: {
      deps: ['parent'],
      cache: false,
      fn: function () {
        var series = this.parent.stats.map(function (stat) {
          return _.pick(stat, ['color', 'data', 'name']);
        })
        return series;
      }
    }
  }
});
