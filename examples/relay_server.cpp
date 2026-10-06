// A lockstep relay: every packet a player sends is forwarded to everyone
// else, and the server tracks who has ended the turn. C++03.
//
//   ./build/relay_server 8080
//   ./build/game_loop 127.0.0.1 8080     (in two terminals)
#include "gn.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>

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

enum NetId {
    NET_END_TURN = 11,   // playerId, turn  (from clients)
    NET_TURN_DONE = 12   // turn            (from the server, once all have ended it)
};

class Relay : public gn::Server {
public:
    Relay() : turn(1) {}

protected:
    virtual void onConnect(gn::Connection c)
    {
        std::printf("player %u joined (%s)\n", c.id(), c.path());
    }

    virtual void onPacket(gn::Connection c, const gn::Packet &p)
    {
        // forward the order unchanged to the other players
        gn::PacketWriter w(p.netId());
        for (size_t i = 0; i < p.size(); i++) w.addValue(p.values()[i]);
        broadcast(w, c);

        if (p.netId() == NET_END_TURN && p.getInt(1) == turn) {
            ended.insert(c.id());
            if (ended.size() == connectionCount()) {
                gn::PacketWriter done(NET_TURN_DONE);
                done.add(turn);
                broadcast(done);
                std::printf("turn %d complete\n", turn);
                ended.clear();
                turn++;
            }
        }
    }

    virtual void onDisconnect(gn::Connection c, int code)
    {
        std::printf("player %u left (%d)\n", c.id(), code);
        ended.erase(c.id());
    }

private:
    int turn;
    std::set<unsigned> ended;
};

int main(int argc, char **argv)
{
    Relay relay;
    unsigned short port = (unsigned short)(argc > 1 ? std::atoi(argv[1]) : 8080);
    if (relay.listen(port) != GN_OK) {
        std::printf("cannot listen on %u\n", port);
        return 1;
    }
    std::printf("relay listening on %u\n", relay.port());
    for (;;) {
        relay.poll();
        sleepMs(1);
    }
}
