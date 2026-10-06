// Tests for gn.hpp. Builds as C++98. Usage: test_cpp <ws-port> <http-port> <closed-port>
#include "gn.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
static void sleepMs(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleepMs(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static int passed = 0, failed = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s: %s (line %d)\n", name, msg, __LINE__); \
            failed++;                                                      \
            return;                                                        \
        }                                                                  \
    } while (0)

static void ok(const char *name)
{
    std::printf("  pass: %s\n", name);
    passed++;
}

static void testWriterGrowsAndRoundTrips()
{
    const char *name = "PacketWriter grows past its initial buffer and round-trips";
    gn::PacketWriter w(42);
    for (int i = 0; i < 300; i++) w.add(i * 1000);
    w.add(std::string("tail")).add(true).add(2.5f).addUndefined();
    CHECK(w.ok(), "writer ok");
    CHECK(w.size() > 256, "should have grown");

    gn_reader r;
    gn_reader_init(&r, w.data() + 2, w.size() - 2);
    gn::Packet p(&r);
    CHECK(p.netId() == 42 && !p.malformed(), "netId");
    CHECK(p.size() == 304, "value count");
    CHECK(p.getInt(299) == 299000, "int");
    CHECK(p.getString(300) == "tail", "string");
    CHECK(p.getBool(301) == true && p.type(301) == GN_U8, "bool as u8");
    CHECK(p.getDouble(302) == 2.5, "float");
    CHECK(!p.has(303) && p.type(303) == GN_UNDEFINED, "undefined");
    CHECK(p.getInt(999, -1) == -1 && p.getString(0, "def") == "def", "defaults");
    ok(name);
}

static void testWriterErrors()
{
    const char *name = "PacketWriter errors are sticky";
    gn::PacketWriter big(70000);
    CHECK(!big.ok() && big.error() == GN_ERR_RANGE && big.data() == 0, "netId range");

    gn::PacketWriter w(1);
    w.add(1).add(5000000000LL).add(2);
    CHECK(!w.ok() && w.error() == GN_ERR_RANGE && w.size() == 0, "int range");

    gn::PacketWriter huge(1);
    std::string s(40000, 'a');
    huge.add(s).add(s);
    CHECK(!huge.ok() && huge.error() == GN_ERR_TOO_LARGE, "payload too large");
    ok(name);
}

class EchoClient : public gn::Client {
public:
    EchoClient() : connected(false), disconnected(false), code(0), got(0), lastInt(0) {}
    bool connected, disconnected;
    int code, got;
    long long lastInt;
    std::string lastString;

protected:
    virtual void onConnect() { connected = true; }
    virtual void onPacket(const gn::Packet &p)
    {
        got++;
        lastInt = p.getInt(0);
        lastString = p.getString(1);
    }
    virtual void onDisconnect(int c)
    {
        disconnected = true;
        code = c;
    }
};

static void testClient(int port)
{
    const char *name = "Client subclass echo";
    EchoClient c;
    CHECK(c.valid(), "create");
    CHECK(c.connect("127.0.0.1", (unsigned short)port) == GN_OK, "connect");
    for (int i = 0; i < 5000 && !c.connected; i++) { c.poll(); sleepMs(1); }
    CHECK(c.isOpen(), "open");

    gn::PacketWriter w(1);
    w.add(-123456).add("from c++");
    CHECK(c.send(w) == GN_OK, "send");
    for (int i = 0; i < 5000 && !c.got; i++) { c.poll(); sleepMs(1); }
    CHECK(c.got == 1 && c.lastInt == -123456 && c.lastString == "from c++", "echo");

    c.close();
    for (int i = 0; i < 3000 && !c.disconnected; i++) { c.poll(); sleepMs(1); }
    CHECK(c.disconnected && c.code == 1000, "closed");
    ok(name);
}

class EchoServer : public gn::Server {
public:
    EchoServer() : connects(0), disconnects(0), lastCode(0) {}
    int connects, disconnects, lastCode;
    std::string lastPath;

protected:
    virtual void onConnect(gn::Connection c)
    {
        connects++;
        lastPath = c.path();
        c.setUser(this);
    }
    virtual void onPacket(gn::Connection c, const gn::Packet &p)
    {
        gn::PacketWriter w(p.netId());
        w.add(p.getInt(0) * 2).add(p.getString(1) + "!");
        if (c.user() == this) c.send(w);
    }
    virtual void onDisconnect(gn::Connection, int code)
    {
        disconnects++;
        lastCode = code;
    }
};

static void testServer()
{
    const char *name = "Server subclass with a Client in the same loop";
    EchoServer s;
    EchoClient c;
    CHECK(s.listen(0, "127.0.0.1") == GN_OK && s.port() != 0, "listen");
    CHECK(c.connect("127.0.0.1", s.port(), "/game") == GN_OK, "connect");
    for (int i = 0; i < 5000 && !(c.connected && s.connects); i++) { s.poll(); c.poll(); sleepMs(1); }
    CHECK(c.isOpen() && s.connects == 1 && s.lastPath == "/game" && s.connectionCount() == 1, "connected");

    gn::PacketWriter w(4);
    w.add(21).add("hi");
    c.send(w);
    for (int i = 0; i < 5000 && !c.got; i++) { s.poll(); c.poll(); sleepMs(1); }
    CHECK(c.got == 1 && c.lastInt == 42 && c.lastString == "hi!", "reply");

    s.close();
    for (int i = 0; i < 5000 && !(c.disconnected && s.disconnects); i++) { s.poll(); c.poll(); sleepMs(1); }
    CHECK(c.code == 1001 && s.disconnects == 1 && s.lastCode == 1001, "server close");
    ok(name);
}

int main(int argc, char **argv)
{
    testWriterGrowsAndRoundTrips();
    testWriterErrors();
    testServer();
    if (argc > 1) testClient(std::atoi(argv[1]));
    std::printf("C++ tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
