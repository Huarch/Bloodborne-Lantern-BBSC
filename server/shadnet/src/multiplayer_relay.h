// BBSC addressed game relay. Native signaling stays on the existing endpoint;
// only the opt-in mod sees logical 198.18.x.x peer addresses (never routed on the Internet).
#pragma once
#include <QDateTime>
#include <QRandomGenerator>
#include <QtEndian>
#include <QUdpSocket>
#include "client_session.h"

class MultiplayerRelay {
    struct Peer { QHostAddress ip; quint16 port = 0; quint32 alias = 0; qint64 seen = 0; };
    SharedState* shared;
    QUdpSocket* socket;
    QHash<QString, Peer> peers;
    QHash<quint32, QString> aliases;
    quint32 nextAlias = 1;
    const quint64 epoch = QRandomGenerator::global()->generate64() | 1;
    static constexpr quint16 LogicalPort = 31314;
    static constexpr qint64 LeaseMs = 15000;
    template<class T> static T Read(const QByteArray& b, int at) {
        return qFromBigEndian<T>(reinterpret_cast<const uchar*>(b.constData() + at));
    }
    template<class T> static void Put(QByteArray& b, T value) {
        const T be = qToBigEndian(value);
        b.append(reinterpret_cast<const char*>(&be), sizeof(be));
    }
    static QString Name(const QByteArray& b, int at) {
        auto n = b.mid(at, 16); const int end = n.indexOf('\0');
        if (end >= 0) n.truncate(end);
        return QString::fromLatin1(n);
    }
    static void PutName(QByteArray& b, const QString& name) {
        auto n = name.toLatin1().left(16); n.resize(16, '\0'); b.append(n);
    }
    QByteArray Reply(char kind) const {
        QByteArray b("BBU2", 4); b.append(kind); Put(b, epoch); return b;
    }
    bool Live(const QString& name, qint64 now) const {
        const auto it = peers.constFind(name);
        return it != peers.constEnd() && now - it->seen <= LeaseMs;
    }
    bool Authorized(const QString& a, const QString& b) const {
        if (a == b) return false;
        bool aInRoom = false, bInRoom = false;
        {
            QReadLocker lock(&shared->matching.roomsLock);
            for (const Room& room : shared->matching.rooms) {
                bool hasA = false, hasB = false;
                for (const auto& m : room.members) {
                    hasA |= m.npid == a; hasB |= m.npid == b;
                }
                if (hasA && hasB) return true;
                aInRoom |= hasA; bInRoom |= hasB;
            }
        }
        // Do not carry game data across two different rooms, even if an old signaling
        // authorization remains. Before joining, the native summon authorizes the pair.
        if (aInRoom && bInRoom) return false;
        QReadLocker lock(&shared->matching.udpLock);
        return shared->matching.bbscRelayPeers.value(a).contains(b);
    }
    void PutEndpoint(QByteArray& b, const QString& target) const {
        PutName(b, target); Put(b, peers.value(target).alias); Put(b, LogicalPort);
    }
public:
    MultiplayerRelay(SharedState* state, QUdpSocket* udp) : shared(state), socket(udp) {}
    bool Handle(const QByteArray& b, const QHostAddress& ip, quint16 port) {
        if (!b.startsWith("BBU2")) return false;
        if (b.size() < 5 || b.size() > 65023) return true;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const auto kind = static_cast<quint8>(b[4]);
        if (kind == 1 && b.size() == 21) {
            const QString name = Name(b, 5);
            if (name.isEmpty()) return true;
            {
                QReadLocker lock(&shared->clientsLock);
                const auto user = shared->npidToUserId.constFind(name);
                if (user == shared->npidToUserId.constEnd() || !shared->clients.contains(*user))
                    return true;
            }
            {
                QReadLocker lock(&shared->matching.udpLock);
                const auto game = shared->matching.udpExt.constFind(name);
                if (game == shared->matching.udpExt.constEnd() || QHostAddress(game->first) != ip)
                    return true;
            }
            auto it = peers.find(name);
            if (it == peers.end()) {
                if (nextAlias > 65534) return true;
                Peer p; p.alias = 0xc6120000U | nextAlias++;
                aliases.insert(p.alias, name); it = peers.insert(name, p);
            }
            const bool fresh = it->ip != ip || it->port != port || now - it->seen > LeaseMs;
            it->ip = ip; it->port = port; it->seen = now;
            QByteArray out = Reply(2); Put(out, it->alias); Put(out, LogicalPort);
            socket->writeDatagram(out, ip, port);
            if (fresh) qInfo() << "BBSC addressed relay registered:" << name;
            return true;
        }
        if (b.size() < 13 || Read<quint64>(b, 5) != epoch) return true;
        QString sender;
        for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
            if (it->ip == ip && it->port == port && now - it->seen <= LeaseMs) {
                sender = it.key(); break;
            }
        }
        if (sender.isEmpty()) return true;
        if (kind == 3 && b.size() == 29) {
            const QString target = Name(b, 13);
            if (!Live(target, now) || !Authorized(sender, target)) return true;
            QByteArray out = Reply(4); PutEndpoint(out, target);
            socket->writeDatagram(out, ip, port);
        } else if (kind == 5 && b.size() == 23) {
            const quint64 roomId = Read<quint64>(b, 13);
            const quint16 memberId = Read<quint16>(b, 21);
            QString target;
            {
                QReadLocker lock(&shared->matching.roomsLock);
                for (const Room& room : shared->matching.rooms) {
                    if (room.roomId != roomId) continue;
                    bool belongs = false;
                    for (const auto& m : room.members) belongs |= m.npid == sender;
                    if (belongs && room.members.contains(memberId))
                        target = room.members.value(memberId).npid;
                }
            }
            if (!Live(target, now) || !Authorized(sender, target)) return true;
            QByteArray out = Reply(6); Put(out, roomId); Put(out, memberId); PutEndpoint(out, target);
            socket->writeDatagram(out, ip, port);
        } else if (kind == 7 && b.size() > 23) {
            const QString target = aliases.value(Read<quint32>(b, 13));
            if (Read<quint16>(b, 17) != LogicalPort || Read<quint16>(b, 19) == 0 ||
                Read<quint16>(b, 21) == 0 || !Live(target, now) || !Authorized(sender, target))
                return true;
            QByteArray out = Reply(8); Put(out, peers.value(sender).alias); Put(out, LogicalPort);
            out.append(b.constData() + 19, b.size() - 19); // src vport, dst vport, payload
            const auto& destination = peers[target];
            socket->writeDatagram(out, destination.ip, destination.port);
        }
        return true;
    }
};
