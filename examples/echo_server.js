// Minimal gn.js server that echoes every packet back.
// Usage: node examples/echo_server.js [gn.js path] [port]
const path = require('path');
const gn = require(path.resolve(process.argv[2] || '../gn.js'));

const server = new gn.Server();
server.on('connect', () => console.log('client connected'));
server.on('packet', (conn, packet) => conn.send(packet));
server.on('disconnect', () => console.log('client disconnected'));
server.listen(Number(process.argv[3] || 8080));
