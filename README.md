# gn.c

C implementation of the [gn](https://github.com/Yotis-Studios/gn.js) binary
protocol, with a WebSocket client, a WebSocket server and a C++ wrapper. It
talks to gn.js and gn.hml clients and servers.

- **Codec**: builds and parses packets in memory. No allocation, no I/O, works
  with any transport.
- **Client and server**: non-blocking `ws://` WebSocket, no dependencies. You
  call `poll()` once per frame, and callbacks run inside that call on your
  thread. No threads are created. The server handles any number of clients,
  so a game can host from its own main loop.
- **C++ wrapper** (`gn.hpp`): header-only and C++03, so it builds with older
  compilers such as VC9-era toolchains.

It's C99, 32- or 64-bit, and runs on Windows (Winsock), Linux and macOS. The
wire format is specified in gn.js's
[PROTOCOL.md](https://github.com/Yotis-Studios/gn.js/blob/main/PROTOCOL.md),
and the codec passes the shared conformance vectors from gn.js.

## Adding it to a project

Copy `include/` and `src/` into your tree and compile the two `.c` files as C99:

```
include/gn.h      C API
include/gn.hpp    C++ wrapper (optional)
src/gn_codec.c    packets
src/gn_ws.c       WebSocket plumbing shared by client and server
src/gn_internal.h   (private header for the three below)
src/gn_client.c   WebSocket client
src/gn_server.c   WebSocket server
```

The codec alone is enough with your own transport; leave out the other three.

- **Windows:** link `ws2_32`. The library doesn't rely on `#pragma comment(lib)`,
  so it also works with `/NODEFAULTLIB`. It avoids C99 library functions that
  old MSVC runtimes lack (`snprintf`, `strcasecmp`), so it builds against the
  VC9 CRT. It needs `<stdint.h>`, which clang/clang-cl supply even when the
  CRT doesn't.
- **POSIX:** no extra libraries are needed.
- **Threads:** none are created, and the library never touches `rand()` or any
  other global state. Use each `gn_client` from one thread.

## C++

```cpp
#include "gn.hpp"

enum { NET_MOVE_UNIT = 10 };

class Net : public gn::Client {
protected:
    virtual void onConnect() { /* ready to send */ }
    virtual void onPacket(const gn::Packet &p) {
        if (p.netId() == NET_MOVE_UNIT)
            moveUnit(p.getInt(0), p.getInt(1), p.getInt(2));
    }
    virtual void onDisconnect(int code) { /* 1000 = normal */ }
    virtual void onError(int err, const char *message) {}
};

Net net;
net.connect("127.0.0.1", 8080);

// every frame:
net.poll();

// sending:
gn::PacketWriter w(NET_MOVE_UNIT);
w.add(unitId).add(x).add(y);
net.send(w);
```

`gn::Packet` is only valid during `onPacket`. `getString()` and `getBuffer()`
return copies, so copy out anything you keep. See
[examples/game_loop.cpp](examples/game_loop.cpp) for a complete program.

### Server

```cpp
class Relay : public gn::Server {
protected:
    virtual void onConnect(gn::Connection c) { /* c.id(), c.path() */ }
    virtual void onPacket(gn::Connection from, const gn::Packet &p) {
        gn::PacketWriter w(p.netId());
        for (size_t i = 0; i < p.size(); i++) w.addValue(p.values()[i]);
        broadcast(w, from);               // to everyone but the sender
    }
    virtual void onDisconnect(gn::Connection c, int code) {}
};

Relay relay;
relay.listen(8080);      // all IPv4 interfaces; listen(0, "127.0.0.1") for a free local port
// every frame:
relay.poll();
```

`gn::Connection` is a handle you can copy and compare, with `send`, `close(code)`,
`id`, `path` and `setUser`/`user`. Don't use it after `onDisconnect` for it returns.
See [examples/relay_server.cpp](examples/relay_server.cpp), a lockstep relay
that also tracks the turn barrier.

## C

```c
#include "gn.h"

static void on_packet(void *user, gn_reader *r) {
    gn_value v;
    while (gn_read(r, &v)) {
        /* v.type is the wire type; v.as.i / v.as.f / v.as.str / v.as.buf */
    }
}

gn_client_callbacks cb = {0};
cb.on_packet = on_packet;
gn_client *c = gn_client_create(&cb);
gn_client_connect(c, "127.0.0.1", 8080, NULL);

/* every frame */
gn_client_poll(c);

/* send */
unsigned char buf[GN_MAX_PACKET];
gn_writer w;
gn_writer_init(&w, buf, sizeof buf, 10 /* netId */);
gn_write_int(&w, 42);
gn_write_string(&w, "hello");
gn_client_send_packet(c, &w); /* fails if any write failed */
```

### Encoding rules

`gn_write_int`, `gn_write_double` and the other writers pick the smallest wire
type, the same way gn.js does:
- Integers go out as u8 through s32.
- Doubles go out as f32 if their magnitude is at most 2²⁴, otherwise f64.
- Booleans go out as u8 1/0.

Out-of-range integers, an over-long string or buffer, an oversized packet, and
a `netId` above 65535 are errors (`GN_ERR_RANGE` / `GN_ERR_TOO_LARGE`), never
silently wrapped. Errors are sticky, so write everything and check once.
`gn_write_value` writes a value with exactly the type it carries.

### Decoding

`gn_read` returns values in order. Strings and buffers point into the
received message: they are not NUL-terminated, and they are only valid during
the callback. Malformed data stops decoding without crashing: `gn_read`
returns 0 with `r->err == GN_ERR_MALFORMED`, and everything read before that
point stays valid.

### Client behavior

- **`gn_client_connect`** returns immediately. The TCP connect and WebSocket
  handshake happen in `gn_client_poll`, then `on_connect` fires. Name
  resolution is the one blocking step, so pass an IP address to avoid it.
- **`on_disconnect`** fires exactly once for every `connect` that returned
  `GN_OK`, whether the connection ended or never opened. It reports the
  server's close code (1000 normal, 1006 lost). You can reconnect from inside
  it.
- **Handled internally:** ping/pong, fragmented messages, server close codes,
  and several packets in one message.
- **Text messages** aren't part of the protocol. They're reported through
  `on_error` and don't close the connection.
- **Limits:** 16 MB per message and a 16 MB send backlog, a 10 s connect
  timeout, and a 2 s close timeout.

### Server behavior (C API: `gn_server_*`, `gn_conn_*`)

- **Connections** become visible in `on_connect` after the WebSocket
  handshake. Requests that aren't valid upgrades get an HTTP error (400, or
  426 for the wrong WebSocket version) and an `on_error` with a NULL `conn`.
- **`on_disconnect`** fires exactly once for every connection that fired
  `on_connect`, with the close code (1000 normal, 1001 server closing, 1006
  lost, or your own 4000-4999 from `gn_conn_close`). A `gn_conn` stays valid
  until that callback returns.
- **Inside callbacks** you can send to any connection, close any connection,
  broadcast, and call `gn_server_close`.
- **`gn_server_close`** stops listening and closes every connection with 1001.
  Their `on_disconnect`s fire from later polls; once
  `gn_server_connection_count()` is 0 you can destroy the server.
- **Clients must mask their frames** (RFC 6455); unmasked frames close the
  connection with 1002. Text messages are reported through `on_error` and the
  connection stays open.
- **Binding:** `host` NULL listens on all IPv4 interfaces. IPv6 needs an
  explicit address. Port 0 picks a free port (`gn_server_port`).

### Not supported

- **`wss://` (TLS).** The transport is plain TCP. For encrypted connections,
  put a TLS-terminating proxy in front of the server for now.

## Testing

```
make test          conformance vectors, client<->server in one process, C++ wrapper
make test-live     against other implementations: the gn.c client vs a gn.js server;
                   gn.js, raw `ws` and raw TCP clients vs the gn.c server (needs node,
                   GNJS=../gn.js); the gn.hml client vs the gn.c server (if hemlock is
                   installed, GNHML=../gn.hml)
make check         test + test-live, plus -m32 and AddressSanitizer/UBSan builds
make fuzz          libFuzzer on the decoder and both WebSocket parsers (clang)
make test-windows  cross-build with WIN_CC (e.g. zig cc -target x86-windows-gnu), run under wine
```

`tests/vectors/protocol.json` is copied from gn.js
(`test/vectors/protocol.json`). Update it from there, never by hand.

## License

MIT
