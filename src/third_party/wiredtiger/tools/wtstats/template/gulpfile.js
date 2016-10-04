var gulp = require('gulp'),
    jade = require('gulp-jade'),
    less = require('gulp-less'),
    uglify = require('gulp-uglify'),
    uglifycss = require('gulp-uglifycss'),
    watchify = require('watchify'),
    livereload = require('gulp-livereload'),
    buffer = require('vinyl-buffer'),
    notifier = require('node-notifier'),
    browserify = require('browserify'),
    packify = require('packify'),
    source = require('vinyl-source-stream'),
    jadeify = require('jadeify'),
    prettyTime = require('pretty-hrtime'),
    gutil = require('gulp-util'),
    fs = require('fs');

var DEBUG = false;
var BUILD = './build';

/**
 * Helper for catching error events on vinyl-source-stream's and showing
 * a nice native notification and printing a cleaner error message to
 * the console.
 */
function createErrorNotifier(titlePrefix) {
  return function(err) {
    var title = titlePrefix + ' error',
      message = err.message;

    if (err.fileName) {
      var filename = err.fileName.replace(__dirname + '/', '');
      title = titlePrefix + ' error' + filename;
    }

    if (err.lineNumber) {
      message = err.lineNumber + ': ' + err.message.split(' in file ')[0].replace(/`/g, '"');
    }

    notifier.notify({
      title: title,
      message: message
    });
    console.log(err);
    gutil.log(gutil.colors.red.bold(title), message);
  };
}

gulp.task('default', ['static', 'templates', 'styles', 'fonts', 'js']);

gulp.task('js', function() {
  var b = browserify({
    entries: ['./app/index.js'],
    debug: DEBUG
  })
    .transform(jadeify)
    .bundle()
    .pipe(source('index.js'));

  // Uglify for compression if not in DEBUG.
  if (!DEBUG) {
    return b.pipe(buffer())
      .pipe(uglify({mangle: { except: ["data"] }}))
      .pipe(gulp.dest(BUILD));
  } else {
    return b.pipe(gulp.dest(BUILD));
  }
});

gulp.task('watch', function() {
  livereload.listen();

  gulp.watch(['app/less/{*,**/*}.less'], ['styles']);
  gulp.watch(['app/index.jade'], ['templates']);

  gulp.watch([BUILD + '/index.js', BUILD + '/css/index.css'])
    .on('change', livereload.changed);

  /**
   * Gulp's [fast browserify builds recipe](http://git.io/iiCk-A)
   */
  var bundler = watchify(browserify('./app/index.js', {
    cache: {},
    packageCache: {},
    fullPaths: true,
    debug: DEBUG

  }))
    .transform('jadeify')
    .on('update', rebundle);

  function rebundle() {
    var start = process.hrtime();
    gutil.log('Starting', '\'' + gutil.colors.cyan('rebundle') + '\'...');
    return bundler.bundle()
      .on('error', createErrorNotifier('js'))
      .pipe(source('index.js'))
      .pipe(gulp.dest(BUILD))
      .on('end', function() {
        var time = prettyTime(process.hrtime(start));
        gutil.log('Finished', '\'' + gutil.colors.cyan('rebundle') + '\'',
          'after', gutil.colors.magenta(time));
      });
  }

  return rebundle();
});

gulp.task('templates', function() {
  gulp.src('./app/index.jade')
    .pipe(jade())
    .pipe(gulp.dest(BUILD));
});

gulp.task('fonts', function() {
  return gulp.src('node_modules/font-awesome/fonts/fontawesome-webfont*')
    .pipe(gulp.dest(BUILD + '/fonts'));
});

gulp.task('styles', function() {
  var opts = {
    compress: true,
    paths: [
      'less',
      'node_modules',
      'node_modules/font-awesome/less',      
      'node_modules/bootstrap/less'
    ]
  };
  return gulp.src('./app/less/index.less')
    .pipe(less(opts))
    .pipe(uglifycss())
    .pipe(gulp.dest(BUILD + '/css'));
});

/**
 * 'gulp pack' will build all targets, then run packify to bundle everything into
 * a single .html file, and save this template in the templates directory.
 */
gulp.task('pack', ['default'], function() {
  process.chdir('./' + BUILD)
  var opts = {
    html: fs.readFileSync('index.html', 'utf8')
  }
  packify(opts, function (err, packed) {
    if (err) return console.error(err);
    fs.writeFileSync('../wtstats.html.template', packed);
  });
});


/**
 * Copies all static asset files into `BUILD`.
 */
gulp.task('static', [
  'copy images'
]);

gulp.task('copy images', function() {
  return gulp.src('./assets/img/*')
    .pipe(gulp.dest(BUILD + '/img'));
});
