var AmpersandState = require('ampersand-state'),
    UnderscoreMixin = require('ampersand-collection-underscore-mixin'),
    AmpersandCollection = require('ampersand-collection'),
    Stat = require('./stat'),
    debug = require('debug')('model:panel');

var StatCollection = AmpersandCollection.extend(UnderscoreMixin, {
  comparator: 'name',
  model: Stat
});

var Panel = module.exports = AmpersandState.extend({
  collections: {
    stats: StatCollection
  },
  props: {
    title: {
      type: 'string',
      required: true
    },
    open: {
      type: 'boolean',
      default: false
    }
  },
  derived: {
    selected: {
      // tri-state: returns 'all', 'some', or 'none'
      deps: ['stats'],
      cache: false,
      fn: function () {
        var selected = stats.filter(function (stat) {
          return stat.selected;
        });
        if (selected.length === stats.length) return 'all';
        if (selected.length === 0) return 'none';
        return 'some';
      }
    }
  },
  selectAll: function () {
    stats.each(function (stat) {
      stat.selected = true;
    });
  },
  deselectAll: function () {
    stats.each(function (stat) {
      stat.selected = false;
    });
  }
});
