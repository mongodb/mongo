var AmpersandView = require('ampersand-view'),
    AmpersandSubCollection = require('ampersand-subcollection'),
    StatView = require('./stat'),
    $ = require('jquery'),
    _ = require('lodash'),
    debug = require('debug')('view:panel');

var PanelView = module.exports = AmpersandView.extend({
  props: {
    indicator: {
      type: 'string',
      default: 'none',
      values: ['none', 'some', 'all']
    },
    statViews: 'object',
    filteredStats: 'object'
  },
  template: require('./templates/panel.jade'),
  events: {
    'click a': 'collapsibleToggle',
    'click [data-hook=caret]': 'collapsibleToggle',
    'click [data-hook=indicator]': 'indicatorClicked'
  },
  bindings: {
    'indicator': {
      type: function (el, val, prev) {
        $el = $(el);
        $el.removeClass();
        switch (this.model.selected) {
          case 'all' : $el.addClass('fa fa-circle'); break;
          case 'some': $el.addClass('fa fa-adjust'); break;
          case 'none': $el.addClass('fa fa-circle-o'); break;
        }
      },
      hook: 'indicator'
    },
    'model.open': {
      type: 'booleanClass',
      hook: 'caret',
      yes: 'fa-caret-up',
      no: 'fa-caret-down'
    }
  },
  initialize: function () {
    this.filteredStats = new AmpersandSubCollection(this.model.stats, {
      comparator: function (model) { return model.name }
    });
  },
  render: function () {
    this.renderWithTemplate(this.model);
    this.statViews = this.renderCollection(this.filteredStats, StatView, this.queryByHook('stats'));
  },
  indicatorClicked: function (event) {
    var all = this.model.selected !== 'all';
    this.filteredStats.each(function (stat) {
      stat.selected = all;
    });
    
    switch (this.model.selected) {
      case 'all': // fall-through
      case 'some': this.collapsibleOpen(); break;
      case 'none': this.collapsibleClose(); break;
    }
    this.model.app.clearSelectionState();
    this.statChanged(null, {propagate: true});
  },
  statChanged: function (stat, options) {
    options = options || {};
    // mirroring model.selected here to use for bindings
    this.indicator = this.model.selected;
    if (options.propagate) {
      this.parent.parent.statChanged(stat, options);
    }
  },
  collapsibleToggle: function (event) {
    if (this.model.open) {
      this.collapsibleClose(event);
    } else {
      this.collapsibleOpen(event);
    }
  },
  collapsibleClose: function (event) {
    $(this.query('.collapse')).collapse('hide');
    this.model.open = false;
  },
  collapsibleOpen: function (event) {
    $(this.query('.collapse')).collapse('show');
    this.model.open = true;
  },
  resetStats: function () {
    this.filteredStats.configure({}, true);
  },
  filterStats: function(search) {
    this.filteredStats.configure({
      filter: function (model) { 
        return (model.name.search(new RegExp(search), 'gi') !== -1); 
      }
    }, true);
    if (this.filteredStats.length === 0) {
      this.collapsibleClose();
    } else {
      this.collapsibleOpen();
    }
  }
});
