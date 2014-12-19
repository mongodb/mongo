var $ = window.$ = window.jQuery = require('jquery');

var AppView = require('../views/app'),
    App = require('../models/app'),
    debug = require('debug')('index');

require('bootstrap/js/dropdown');
require('bootstrap/js/collapse');
require('bootstrap/js/transition');
require('bootstrap/js/button');

// var data = "### INSERT DATA HERE ###";
var data = require('../fixtures/bigger.fixture.json');

var app = window.app = new App(data, {parse: true});

var el = document.getElementById('app');
var view = new AppView({model: app, el: el});

view.render();
