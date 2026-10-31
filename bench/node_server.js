// The Node.js equivalent of the Ark app, for benchmarking: same routes, same responses.
// Usage: node bench/node_server.js [port]
const http = require('http');

const port = Number(process.argv[2] || 8282);

http.createServer((req, res) => {
    let status = 200;
    let body = 'Hello from Ark\n';

    if (req.method !== 'GET') {
        status = 405;
        body = 'Method Not Allowed\n';
    } else if (req.url !== '/') {
        status = 404;
        body = 'Not Found\n';
    }

    res.writeHead(status, { 'Content-Type': 'text/plain', 'Content-Length': Buffer.byteLength(body) });
    res.end(body);
}).listen(port);
