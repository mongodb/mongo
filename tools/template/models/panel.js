var AmpersandState = require('ampersand-state'),
    StatCollection = require('./stat-collection'),
    debug = require('debug')('model:panel');

var Panel = module.exports = AmpersandState.extend({
  collections: {
    stats: StatCollection,
  },
  props: {
    title: {
      type: 'string',
      required: true
    },
    open: {
      type: 'boolean',
      default: false
    },
  },
  derived: {
    selected: {
      // tri-state: returns 'all', 'some', or 'none'
      deps: ['stats'],
      cache: false,
      fn: function () {
        var selected = this.stats.filter(function (stat) {
          return stat.selected;
        });
        if (selected.length === this.stats.length) return 'all';
        if (selected.length === 0) return 'none';
        return 'some';
      }
    }
  }
});
