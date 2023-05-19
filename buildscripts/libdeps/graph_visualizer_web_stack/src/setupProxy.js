/**
 * This proxy is intended to allow the visualizer to run in a development environment
 * which includes SSH tunnels communicating with private remote hosts.
 */

const { createProxyMiddleware } = require('http-proxy-middleware');

module.exports = function(app) {
    app.use(
        createProxyMiddleware('/api', {
            target: 'http://localhost:5000',
            changeOrigin: true,
            secure: false,
        })
    );
};
