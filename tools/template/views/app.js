var AmpersandView = require('ampersand-view'),
    SidebarView = require('./sidebar'),
    ChartView = require('./chart'),
    $ = require('jquery'),
    debug = require('debug')('view:app');

var AppView = module.exports = AmpersandView.extend({
  template: require('./templates/app.jade'),
  props: {
    chartView: 'object',
    menuOpen: {
      type: 'boolean',
      default: false
    }
  },
  events: {
    'click .navbar-right label': 'navButtonClicked',
    'click [data-hook=menu-toggle]': 'menuToggled'
  },
  bindings: {
    'model.chart.subSampled': {
      type: 'toggle',
      hook: 'sample-warning'
    },
    'menuOpen': {
      type: 'booleanClass',
      selector: '#wrapper',
      name: 'active'
    }
  },
  subviews: {
    sidebar: {
      hook: 'sidebar',
      waitFor: 'model.sidebar',
      prepareView: function (el) {
        return new SidebarView({
          el: el,
          model: this.model.sidebar
        })
      }
    },
    chart: {
      hook: 'chart',
      waitFor: 'model.chart',
      prepareView: function (el) {
        this.chartView = new ChartView({
          el: el,
          model: this.model.chart
        });
        return this.chartView;
      }
    }
  },
  statChanged: function (stat, options) {
    this.model.chart.recalcXDomain = true;
    this.chartView.render();
  },
  render: function () {
    this.renderWithTemplate(this.model);
  },
  navButtonClicked: function (event) {
    var $input = $(event.target).find('input');
    var name = $input.attr('name');
    var value = $input.val();
    this.model.chart[name] = value;
    this.model.chart.recalcXDomain = (name === 'xSetting');
    this.chartView.render();
  },
  menuToggled: function (event) {
    this.toggle('menuOpen');
  }

});
