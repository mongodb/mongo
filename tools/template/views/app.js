var AmpersandView = require('ampersand-view'),
    SidebarView = require('./sidebar'),
    ChartView = require('./chart'),
    $ = require('jquery'),
    debug = require('debug')('view:app');

var AppView = module.exports = AmpersandView.extend({
  template: require('./templates/app.jade'),
  props: {
    chartView: 'object'
  },
  events: {
    'click .navbar-right label': 'navButtonClicked'
  },
  bindings: {
    'model.chart.subSampled': {
      type: 'toggle',
      hook: 'sample-warning'
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
  statChanged: function (stat) {
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
  }

  // navButtonClicked: function (event) {
  //   var $target = $(event.target);
  //   var text = $target.text();

  //   if (text === 'linear' || text === 'log-scale') {
  //     this.model.yLinear = (text === 'linear');
  //     $(this.queryByHook('linear')).removeClass('active');
  //     $(this.queryByHook('absolute')).removeClass('active');

  //   }


  //   switch (text) {
  //     case 'linear': this.model.yLinear = true; break;
  //     case 'log-scale': this.model.yLinear = false; break;
  //     case 'relative': this.model.xRelative = true; break;
  //     case 'absolute': this.model.xRelative = false; break;
  //   }
  // }

});
