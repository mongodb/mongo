var $ = window.$ = window.jQuery = require('jquery');

var AppView = require('../views/app'),
    App = require('../models/app'),
    debug = require('debug')('index');

require('bootstrap/js/dropdown');
require('bootstrap/js/collapse');
require('bootstrap/js/transition');
require('bootstrap/js/button');

// do not change this line, wtstats.py requires this placeholder
var data = "### INSERT DATA HERE ###";

// un-comment line below to use fixture data for development/testing
// var data = require('../fixtures/strdates.fixture.json');

// create main app model and view and render
var app = window.app = new App(data, {parse: true});
var el = document.getElementById('app');
var view = new AppView({model: app, el: el});

view.render();
