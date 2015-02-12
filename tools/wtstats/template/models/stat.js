var AmpersandState = require('ampersand-state'),
    colors = require('./colors').getInstance(),
    debug = require('debug')('model:stat');

var Stat = module.exports = AmpersandState.extend({
  props: {
    name: {
      type: 'string',
      default: ''
    },
    group: {
      type: 'string',
      default: ''
    },
    selected: {
      type: 'boolean',
      default: false
    }, 
    data: {
      type: 'object'
    },
    app: {
      type: 'object'
    }
  },
  derived: {
    color: {
      cache: true,
      fn: function () {
        return colors(this.cid);
      }
    }
  }
});
