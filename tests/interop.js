// Interop tests against a gn.c server (tests/server_peer.c), from gn.js
// clients, a raw `ws` client and raw TCP.
// Usage: node tests/interop.js <server_peer binary> <gn.js path> [runner words...]
const path = require('path');
const net = require('net');
const { spawn } = require('child_process');

const gnjs = path.resolve(process.argv[3]);
const gn = require(gnjs);
const WebSocket = require(require.resolve('ws', { paths: [gnjs] }));

// optional prefix, e.g. `wine` or `setarch x86_64 -R`
const command = process.argv.slice(4).concat([process.argv[2], '0']);
const proc = spawn(command[0], command.slice(1), { stdio: ['ignore', 'pipe', 'inherit'] });
let log = '';
let port = 0;
const waiters = [];
proc.stdout.on('data', (d) => {
    log += d.toString();
    const m = log.match(/READY (\d+)/);
    if (m) port = Number(m[1]);
    for (const w of waiters.splice(0)) w();
});
const logged = (re, ms = 5000) => new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('server never logged ' + re)), ms);
    const check = () => (re.test(log) ? (clearTimeout(t), resolve()) : waiters.push(check));
    check();
});
const timeout = (p, ms, what) => Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error('timeout: ' + what)), ms))]);

function gnClient() {
    const c = new gn.Client();
    c.on('error', () => {});
    return new Promise((resolve) => { c.on('connect', () => resolve(c)); c.connect('127.0.0.1', port); });
}
const nextPacket = (c) => new Promise((resolve) => c.once('packet', resolve));
function rawWs(p = '/') {
    const ws = new WebSocket(`ws://127.0.0.1:${port}${p}`);
    ws.binaryType = 'nodebuffer';
    return new Promise((resolve, reject) => { ws.on('open', () => resolve(ws)); ws.on('error', reject); });
}
function packet(netId, values) { const p = new gn.Packet(netId); p.add(values); return p.build(); }
function rawHttp(request) {
    return new Promise((resolve) => {
        const s = net.connect(port, '127.0.0.1', () => s.write(request));
        let data = Buffer.alloc(0);
        s.on('data', (d) => { data = Buffer.concat([data, d]); });
        s.on('close', () => resolve(data));
        s.on('error', () => resolve(data));
        setTimeout(() => s.destroy(), 3000);
    });
}

const tests = {
    async 'gn.js client: echo of every value type'() {
        const c = await gnClient();
        const p = new gn.Packet(1);
        p.add([200, -40000, 4000000000, 1.5, 'héllo', Buffer.from([1, 2, 255]), undefined, true]);
        const echo = nextPacket(c);
        c.send(p);
        const got = await timeout(echo, 3000, 'echo');
        const want = [200, -40000, 4000000000, 1.5, 'héllo', Buffer.from([1, 2, 255]), undefined, 1];
        if (got.netId !== 1 || got.data.length !== want.length) throw new Error('shape ' + JSON.stringify(got.data));
        want.forEach((w, i) => {
            const g = got.data[i];
            if (Buffer.isBuffer(w) ? Buffer.compare(g, w) !== 0 : g !== w) throw new Error(`value ${i}: ${g}`);
        });
        c.disconnect();
    },
    async 'gn.js clients: broadcast skips the sender'() {
        const a = await gnClient(), b = await gnClient();
        let aGot = 0;
        a.on('packet', () => aGot++);
        const bGets = nextPacket(b);
        a.send(Object.assign(new gn.Packet(2), { data: [7] }));
        const got = await timeout(bGets, 3000, 'broadcast');
        if (got.get(0) !== 7) throw new Error('payload');
        await new Promise((r) => setTimeout(r, 100));
        if (aGot !== 0) throw new Error('sender got its own broadcast');
        a.disconnect(); b.disconnect();
    },
    async 'path is reported'() {
        const ws = await rawWs('/lobby/3');
        await logged(/CONNECT \d+ \/lobby\/3/);
        ws.close();
    },
    async 'text message is an error, connection survives'() {
        const ws = await rawWs();
        ws.send('hello');
        await logged(/ERROR \d+ 4 received a text message/);
        const echo = new Promise((r) => ws.once('message', r));
        ws.send(packet(1, [5]));
        await timeout(echo, 3000, 'echo after text');
        ws.close();
    },
    async 'answers ping'() {
        const ws = await rawWs();
        const pong = new Promise((r) => ws.once('pong', r));
        ws.ping('abc');
        const data = await timeout(pong, 3000, 'pong');
        if (data.toString() !== 'abc') throw new Error('pong payload');
        ws.close();
    },
    async 'reassembles a fragmented message'() {
        const ws = await rawWs();
        const whole = Buffer.concat([packet(1, ['frag', 99]), packet(1, ['ment', 100])]);
        const got = [];
        const both = new Promise((r) => ws.on('message', (m) => { got.push(m); if (got.length === 1) r(); }));
        ws.send(whole.subarray(0, 5), { fin: false });
        ws.send(whole.subarray(5, 12), { fin: false });
        ws.send(whole.subarray(12), { fin: true });
        await timeout(both, 3000, 'echo');
        await new Promise((r) => setTimeout(r, 100));
        const all = Buffer.concat(got);
        if (Buffer.compare(all, whole) !== 0) throw new Error('echo differs: ' + all.toString('hex'));
        ws.close();
    },
    async 'close code from the server'() {
        const ws = await rawWs();
        const closed = new Promise((r) => ws.on('close', (code) => r(code)));
        ws.send(packet(3, []));
        const code = await timeout(closed, 3000, 'close');
        if (code !== 4002) throw new Error('code ' + code);
    },
    async 'abrupt client drop is 1006'() {
        const ws = await rawWs();
        await logged(/CONNECT (\d+)/);
        const before = (log.match(/DISCONNECT \d+ 1006/g) || []).length;
        ws.terminate();
        await timeout(new Promise((r) => {
            const t = setInterval(() => {
                if ((log.match(/DISCONNECT \d+ 1006/g) || []).length > before) { clearInterval(t); r(); }
            }, 20);
        }), 3000, '1006');
    },
    async 'plain HTTP request gets 400'() {
        const res = (await rawHttp('GET / HTTP/1.1\r\nHost: x\r\n\r\n')).toString();
        if (!res.startsWith('HTTP/1.1 400')) throw new Error(res);
    },
    async 'wrong WebSocket version gets 426'() {
        const res = (await rawHttp('GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
            'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n')).toString();
        if (!res.startsWith('HTTP/1.1 426')) throw new Error(res);
    },
    async 'unmasked client frame is a protocol error (1002)'() {
        const res = await rawHttp('GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
            'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n' +
            '\x82\x00');
        const text = res.toString('latin1');
        if (!text.startsWith('HTTP/1.1 101') || !text.includes('s3pPLMBiTxaQ9kYGzzhZRbK+xOo=')) throw new Error('handshake: ' + text);
        const frame = res.subarray(res.indexOf('\r\n\r\n') + 4);
        if (frame[0] !== 0x88 || frame.readUInt16BE(2) !== 1002) throw new Error('close frame ' + frame.toString('hex'));
    },
};

(async () => {
    await logged(/READY \d+/);
    let passed = 0, failed = 0;
    for (const [name, fn] of Object.entries(tests)) {
        try {
            await timeout(fn(), 8000, name);
            console.log('  pass: ' + name);
            passed++;
        } catch (e) {
            console.log(`  FAIL ${name}: ${e.message}`);
            failed++;
        }
    }
    console.log(`Interop tests: ${passed} passed, ${failed} failed`);
    proc.kill();
    process.exit(failed ? 1 : 0);
})();
