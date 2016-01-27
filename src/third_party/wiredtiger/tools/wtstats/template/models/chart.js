var AmpersandState = require('ampersand-state'),
    _ = require('lodash'),
    debug = require('debug')('model:chart');

var Chart = module.exports = AmpersandState.extend({
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
    },
    allowSampling: {
      type: 'boolean',
      default: true
    },
    subSampled: {
      type: 'boolean',
      default: false
    },
    recalcXDomain: {
      type: 'boolean',
      default: true
    }
  },
  derived: {
    // proxy stats from parent
    series: {
      deps: ['parent'],
      cache: false,
      fn: function () {
        var series = this.parent.stats
          .filter(function (stat) {
            return stat.selected;
          })
          .map(function (stat) {
            return _.pick(stat, ['color', 'data', 'name', 'cid', 'selected']);
          })
        return series;
      }
    },
    empty: {
      deps: ['parent', 'series'],
      cache: false,
      fn: function () {
        return this.series.length === 0;
      }
    }
  }
});
