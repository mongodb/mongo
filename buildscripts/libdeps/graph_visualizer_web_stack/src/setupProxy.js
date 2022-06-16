/**
 * This proxy is intended to allow the visualizer to run in a development environment
 * which includes SSH tunnels communicating with private remote hosts.
 */

const { createProxyMiddleware } = require('http-proxy-middleware');

module.exports = function(app) {
    app.use(
        createProxyMiddleware('/socket.io', {
            target: 'http://localhost:5000',
            ws: true,
            changeOrigin: true,
            secure: false
        })
    );
};