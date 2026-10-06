// Test server for tests/test_client.c, built on gn.js.
// Usage: node tests/server.js <gn.js path> <port> <http-port>
//
// Behaviour is chosen by the netId of each packet the client sends:
//   1  echo the packet back
//   2  reply with two packets in one WebSocket message
//   3  reply with a text message (not part of the protocol)
//   4  close the connection with code 4000
//   5  ping the client; reply netId 5 once its pong arrives
//   6  reply with one ~100 KB message of 2000 packets (64-bit frame length)
//   7  reply with a packet split across three WebSocket fragments
// <http-port> serves plain HTTP 200 so the client's handshake check can fail.
const path = require('path');
const http = require('http');

const gn = require(path.resolve(process.argv[2]));
const port = Number(process.argv[3]);
const httpPort = Number(process.argv[4]);

function packet(netId, values) {
    const p = new gn.Packet(netId);
    p.add(values);
    return p.build();
}

const server = new gn.Server();
server.on('packet', (conn, p) => {
    const ws = conn.ws;
    switch (p.netId) {
    case 1:
        conn.send(p);
        break;
    case 2:
        ws.send(Buffer.concat([packet(21, ['first']), packet(22, ['second'])]));
        break;
    case 3:
        ws.send('hello');
        conn.send(packet(3, ['after text']));
        break;
    case 4:
        ws.close(4000, 'bye');
        break;
    case 5:
        ws.once('pong', (data) => conn.send(packet(5, [data.toString()])));
        ws.ping('gn-ping');
        break;
    case 6: {
        const parts = [];
        for (let i = 0; i < 2000; i++) parts.push(packet(6, [i, 'x'.repeat(40)]));
        ws.send(Buffer.concat(parts));
        break;
    }
    case 7: {
        const whole = packet(7, ['fragmented', 123456]);
        ws.send(whole.subarray(0, 3), { fin: false });
        ws.send(whole.subarray(3, 9), { fin: false });
        ws.send(whole.subarray(9), { fin: true });
        break;
    }
    }
});
server.on('error', () => {});
server.on('ready', () => {
    http.createServer((req, res) => {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end('not a websocket server');
    }).listen(httpPort, '127.0.0.1', () => console.log('READY'));
});
server.listen(port);
