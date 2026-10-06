// A turn-based game client in the shape of a real main loop: poll the network
// once per frame, send orders as packets, and apply orders received from the
// server. C++03.
//
//   node examples/echo_server.js ../gn.js 8080   (echoes every packet)
//   ./build/game_loop 127.0.0.1 8080
#include "gn.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
static void sleepMs(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleepMs(int ms)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = (long)ms * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

// One netId per message kind, shared with the server.
enum NetId {
    NET_MOVE_UNIT = 10,  // unitId, x, y
    NET_END_TURN = 11    // playerId, turn
};

class GameNet : public gn::Client {
public:
    GameNet() : running(true) {}
    bool running;

    void moveUnit(int unitId, int x, int y)
    {
        gn::PacketWriter w(NET_MOVE_UNIT);
        w.add(unitId).add(x).add(y);
        send(w);
    }

    void endTurn(int playerId, int turn)
    {
        gn::PacketWriter w(NET_END_TURN);
        w.add(playerId).add(turn);
        send(w);
    }

protected:
    virtual void onConnect() { std::printf("connected\n"); }

    virtual void onPacket(const gn::Packet &p)
    {
        switch (p.netId()) {
        case NET_MOVE_UNIT:
            std::printf("move unit %lld to (%lld, %lld)\n", p.getInt(0), p.getInt(1), p.getInt(2));
            break;
        case NET_END_TURN:
            std::printf("player %lld ended turn %lld\n", p.getInt(0), p.getInt(1));
            close();
            break;
        default:
            std::printf("unknown message %u\n", p.netId());
        }
    }

    virtual void onDisconnect(int code)
    {
        std::printf("disconnected (%d)\n", code);
        running = false;
    }

    virtual void onError(int, const char *message) { std::printf("error: %s\n", message); }
};

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned short port = (unsigned short)(argc > 2 ? std::atoi(argv[2]) : 8080);

    GameNet net;
    if (net.connect(host, port) != GN_OK) {
        std::printf("could not start connecting to %s:%u\n", host, port);
        return 1;
    }

    bool sent = false;
    while (net.running) {   // the game's frame loop
        net.poll();          // all networking happens here
        if (net.isOpen() && !sent) {
            net.moveUnit(7, 12, 30);
            net.endTurn(1, 42);
            sent = true;
        }
        sleepMs(16);         // ~60 fps
    }
    return 0;
}
