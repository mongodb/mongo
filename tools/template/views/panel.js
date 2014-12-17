var AmpersandView = require('ampersand-view'),
    AmpersandSubCollection = require('ampersand-subcollection'),
    StatView = require('./stat'),
    $ = require('jquery'),
    debug = require('debug')('view:panel');

var PanelView = module.exports = AmpersandView.extend({
  props: {
    statViews: 'object',
    filteredStats: 'object'
  },
  template: require('./templates/panel.jade'),
  events: {
    'click a': 'collapsibleToggle'
  },
  bindings: {
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
  collapsibleToggle: function (event) {
    $(this.query('.collapse')).collapse('toggle');
    this.model.toggle('open');
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
