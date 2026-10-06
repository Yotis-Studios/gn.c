/*
 * gn.hpp - C++ wrapper for gn.c. Header-only, C++03 (builds with VC9-era
 * compilers): no exceptions thrown, no C++11 features.
 *
 *   gn::PacketWriter w(MSG_MOVE);
 *   w.add(unitId).add(x).add(y);
 *   client.send(w);
 *
 *   class Net : public gn::Client {
 *       virtual void onPacket(const gn::Packet &p) {
 *           switch (p.netId()) {
 *           case MSG_MOVE: move(p.getInt(0), p.getInt(1), p.getInt(2)); break;
 *           }
 *       }
 *   };
 */
#ifndef GN_HPP
#define GN_HPP

#include "gn.h"

#include <string>
#include <vector>

namespace gn {

/* Builds one packet. Starts with a small buffer and grows as needed (up to
 * the protocol maximum). Errors are sticky; check ok() (or the result of
 * Client::send) once at the end. */
class PacketWriter {
public:
    explicit PacketWriter(unsigned netId) : buf_(256), len_(0)
    {
        gn_writer_init(&w_, &buf_[0], buf_.size(), netId);
    }

#define GN_WRITE_(call)                                       \
    do {                                                      \
        int r_;                                               \
        do { r_ = (call); } while (r_ == GN_ERR_NO_SPACE && grow()); \
    } while (0)

    PacketWriter &add(int v) { GN_WRITE_(gn_write_int(&w_, v)); return *this; }
    PacketWriter &add(unsigned v) { GN_WRITE_(gn_write_uint(&w_, v)); return *this; }
    PacketWriter &add(long v) { GN_WRITE_(gn_write_int(&w_, v)); return *this; }
    PacketWriter &add(unsigned long v) { GN_WRITE_(gn_write_uint(&w_, v)); return *this; }
    PacketWriter &add(long long v) { GN_WRITE_(gn_write_int(&w_, v)); return *this; }
    PacketWriter &add(unsigned long long v) { GN_WRITE_(gn_write_uint(&w_, v)); return *this; }
    PacketWriter &add(float v) { GN_WRITE_(gn_write_double(&w_, v)); return *this; }
    PacketWriter &add(double v) { GN_WRITE_(gn_write_double(&w_, v)); return *this; }
    PacketWriter &add(bool v) { GN_WRITE_(gn_write_bool(&w_, v ? 1 : 0)); return *this; }
    PacketWriter &add(const char *s) { GN_WRITE_(gn_write_string(&w_, s)); return *this; }
    PacketWriter &add(const std::string &s) { GN_WRITE_(gn_write_stringn(&w_, s.data(), s.size())); return *this; }
    PacketWriter &addBuffer(const void *data, size_t len) { GN_WRITE_(gn_write_buffer(&w_, data, len)); return *this; }
    PacketWriter &addUndefined() { GN_WRITE_(gn_write_undefined(&w_)); return *this; }
    PacketWriter &addValue(const gn_value &v) { GN_WRITE_(gn_write_value(&w_, &v)); return *this; }

#undef GN_WRITE_

    bool ok() const { return w_.err == GN_OK; }
    int error() const { return w_.err; }

    /* The finished packet bytes (size header included); NULL if !ok(). */
    const unsigned char *data() { finish(); return ok() ? &buf_[0] : 0; }
    size_t size() { finish(); return len_; }

    gn_writer *raw() { return &w_; }

private:
    void finish() { len_ = gn_writer_finish(&w_); }

    /* After GN_ERR_NO_SPACE: enlarge the buffer and clear the error so the
     * write can be retried. False once at the protocol maximum. */
    bool grow()
    {
        size_t cap = buf_.size() * 2;
        if (buf_.size() >= GN_MAX_PACKET) return false;
        if (cap > GN_MAX_PACKET) cap = GN_MAX_PACKET;
        buf_.resize(cap);
        w_.data = &buf_[0];
        w_.cap = cap;
        w_.err = GN_OK;
        return true;
    }

    std::vector<unsigned char> buf_;
    gn_writer w_;
    size_t len_;

    PacketWriter(const PacketWriter &);
    PacketWriter &operator=(const PacketWriter &);
};

/* A received packet with its values decoded up front. Strings and buffers
 * point into the received message: a Packet is only valid during
 * Client::onPacket. Copy out anything you keep (getString copies). */
class Packet {
public:
    explicit Packet(gn_reader *r) : netId_(r->net_id), hasNetId_(r->has_net_id != 0)
    {
        gn_value v;
        while (gn_read(r, &v)) {
            values_.push_back(v);
        }
        malformed_ = r->err != GN_OK;
    }

    unsigned netId() const { return netId_; }
    bool hasNetId() const { return hasNetId_; }
    /* True if decoding stopped early on bad data; values() still holds what
     * came before it. */
    bool malformed() const { return malformed_; }
    size_t size() const { return values_.size(); }
    const std::vector<gn_value> &values() const { return values_; }

    /* Missing index or incompatible type -> the default. */
    gn_type type(size_t i) const { return i < values_.size() ? values_[i].type : GN_UNDEFINED; }
    bool has(size_t i) const { return i < values_.size() && values_[i].type != GN_UNDEFINED; }

    long long getInt(size_t i, long long def = 0) const
    {
        int64_t v;
        return i < values_.size() && gn_value_int(&values_[i], &v) ? (long long)v : def;
    }
    double getDouble(size_t i, double def = 0.0) const
    {
        double v;
        return i < values_.size() && gn_value_double(&values_[i], &v) ? v : def;
    }
    bool getBool(size_t i, bool def = false) const
    {
        int v;
        return i < values_.size() && gn_value_bool(&values_[i], &v) ? v != 0 : def;
    }
    std::string getString(size_t i, const std::string &def = std::string()) const
    {
        if (i >= values_.size() || values_[i].type != GN_STRING) return def;
        return std::string(values_[i].as.str.ptr, values_[i].as.str.len);
    }
    /* Copies a buffer value; empty if missing or not a buffer. */
    std::vector<unsigned char> getBuffer(size_t i) const
    {
        if (i >= values_.size() || values_[i].type != GN_BUFFER) return std::vector<unsigned char>();
        return std::vector<unsigned char>(values_[i].as.buf.ptr, values_[i].as.buf.ptr + values_[i].as.buf.len);
    }

private:
    unsigned netId_;
    bool hasNetId_;
    bool malformed_;
    std::vector<gn_value> values_;
};

/* WebSocket client. Subclass and override the on* handlers; call poll() once
 * per frame. Handlers run inside poll() on the calling thread. */
class Client {
public:
    Client() : c_(0)
    {
        gn_client_callbacks cb;
        cb.on_connect = &Client::connectThunk;
        cb.on_packet = &Client::packetThunk;
        cb.on_disconnect = &Client::disconnectThunk;
        cb.on_error = &Client::errorThunk;
        cb.user = this;
        c_ = gn_client_create(&cb);
    }
    virtual ~Client() { gn_client_destroy(c_); }

    /* False if creation failed (out of memory / Winsock unavailable). */
    bool valid() const { return c_ != 0; }

    int connect(const char *host, unsigned short port, const char *path = 0)
    {
        return c_ ? gn_client_connect(c_, host, port, path) : GN_ERR_MEMORY;
    }
    void poll() { if (c_) gn_client_poll(c_); }
    void close() { if (c_) gn_client_close(c_); }

    int send(PacketWriter &w) { return c_ ? gn_client_send_packet(c_, w.raw()) : GN_ERR_MEMORY; }
    int send(const void *data, size_t len) { return c_ ? gn_client_send(c_, data, len) : GN_ERR_MEMORY; }

    gn_client_state state() const { return gn_client_get_state(c_); }
    bool isOpen() const { return state() == GN_OPEN; }

protected:
    virtual void onConnect() {}
    virtual void onPacket(const Packet &) {}
    virtual void onDisconnect(int /*code*/) {}
    virtual void onError(int /*err*/, const char * /*message*/) {}

private:
    static void connectThunk(void *u) { static_cast<Client *>(u)->onConnect(); }
    static void packetThunk(void *u, gn_reader *r)
    {
        Packet p(r);
        static_cast<Client *>(u)->onPacket(p);
    }
    static void disconnectThunk(void *u, int code) { static_cast<Client *>(u)->onDisconnect(code); }
    static void errorThunk(void *u, int err, const char *m) { static_cast<Client *>(u)->onError(err, m); }

    gn_client *c_;

    Client(const Client &);
    Client &operator=(const Client &);
};

/* A connected client, as seen by a Server. A cheap handle: copy it freely,
 * but don't use it after Server::onDisconnect for it returns. */
class Connection {
public:
    Connection() : c_(0) {}
    explicit Connection(gn_conn *c) : c_(c) {}

    int send(PacketWriter &w) { return gn_conn_send_packet(c_, w.raw()); }
    int send(const void *data, size_t len) { return gn_conn_send(c_, data, len); }
    void close(int code = 1000) { gn_conn_close(c_, code); }
    unsigned id() const { return gn_conn_id(c_); }
    const char *path() const { return gn_conn_path(c_); }
    void setUser(void *user) { gn_conn_set_user(c_, user); }
    void *user() const { return gn_conn_get_user(c_); }
    gn_conn *raw() const { return c_; }

    bool operator==(const Connection &o) const { return c_ == o.c_; }
    bool operator!=(const Connection &o) const { return c_ != o.c_; }

private:
    gn_conn *c_;
};

/* WebSocket server. Subclass and override the on* handlers; call poll() once
 * per frame. Handlers run inside poll() on the calling thread. */
class Server {
public:
    Server() : s_(0)
    {
        gn_server_callbacks cb;
        cb.on_connect = &Server::connectThunk;
        cb.on_packet = &Server::packetThunk;
        cb.on_disconnect = &Server::disconnectThunk;
        cb.on_error = &Server::errorThunk;
        cb.user = this;
        s_ = gn_server_create(&cb);
    }
    virtual ~Server() { gn_server_destroy(s_); }

    bool valid() const { return s_ != 0; }

    /* host NULL = all IPv4 interfaces; port 0 = pick one (see port()). */
    int listen(unsigned short port, const char *host = 0)
    {
        return s_ ? gn_server_listen(s_, host, port) : GN_ERR_MEMORY;
    }
    unsigned short port() const { return gn_server_port(s_); }
    void poll() { if (s_) gn_server_poll(s_); }
    void close() { if (s_) gn_server_close(s_); }
    size_t connectionCount() const { return gn_server_connection_count(s_); }

    int broadcast(PacketWriter &w, Connection except = Connection())
    {
        return s_ ? gn_server_broadcast_packet(s_, w.raw(), except.raw()) : GN_ERR_MEMORY;
    }
    int broadcast(const void *data, size_t len, Connection except = Connection())
    {
        return s_ ? gn_server_broadcast(s_, data, len, except.raw()) : GN_ERR_MEMORY;
    }

protected:
    virtual void onConnect(Connection) {}
    virtual void onPacket(Connection, const Packet &) {}
    virtual void onDisconnect(Connection, int /*code*/) {}
    /* conn is empty (raw() == NULL) for errors not tied to a client. */
    virtual void onError(Connection, int /*err*/, const char * /*message*/) {}

private:
    static void connectThunk(void *u, gn_conn *c) { static_cast<Server *>(u)->onConnect(Connection(c)); }
    static void packetThunk(void *u, gn_conn *c, gn_reader *r)
    {
        Packet p(r);
        static_cast<Server *>(u)->onPacket(Connection(c), p);
    }
    static void disconnectThunk(void *u, gn_conn *c, int code) { static_cast<Server *>(u)->onDisconnect(Connection(c), code); }
    static void errorThunk(void *u, gn_conn *c, int err, const char *m) { static_cast<Server *>(u)->onError(Connection(c), err, m); }

    gn_server *s_;

    Server(const Server &);
    Server &operator=(const Server &);
};

} /* namespace gn */

#endif /* GN_HPP */
