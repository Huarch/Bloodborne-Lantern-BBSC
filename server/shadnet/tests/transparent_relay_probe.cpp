#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QtEndian>
#include <QUdpSocket>

#include "stun_server.h"

namespace {

bool WaitForDatagram(QUdpSocket& socket, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!socket.hasPendingDatagrams() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return socket.hasPendingDatagrams();
}

QByteArray Register(const char* name) {
    QByteArray packet("BBU1", 4);
    packet.append(static_cast<char>(0x01));
    QByteArray id(name);
    id.resize(16, '\0');
    packet.append(id);
    return packet;
}

QByteArray SendFrame(uint16_t targetPort, uint16_t vport, const QByteArray& payload) {
    QByteArray packet("BBU1", 4);
    packet.append(static_cast<char>(0x03));
    const uint32_t ip = qToBigEndian(QHostAddress(QHostAddress::LocalHost).toIPv4Address());
    const uint16_t port = qToBigEndian(targetPort);
    const uint16_t vp = qToBigEndian(vport);
    packet.append(reinterpret_cast<const char*>(&ip), 4);
    packet.append(reinterpret_cast<const char*>(&port), 2);
    packet.append(reinterpret_cast<const char*>(&vp), 2);
    packet.append(payload);
    return packet;
}

bool CheckDirection(QUdpSocket& sender, uint16_t targetGamePort, QUdpSocket& receiver,
                    uint16_t expectedSourceGamePort, const QByteArray& payload) {
    const QByteArray packet = SendFrame(targetGamePort, 40, payload);
    if (sender.writeDatagram(packet, QHostAddress::LocalHost, 41314) != packet.size() ||
        !WaitForDatagram(receiver, 1000))
        return false;
    QByteArray received;
    received.resize(static_cast<int>(receiver.pendingDatagramSize()));
    receiver.readDatagram(received.data(), received.size());
    if (received.size() != 13 + payload.size() || received.left(5) != QByteArray("BBU1\x04", 5))
        return false;
    const uint16_t sourcePort = qFromBigEndian<uint16_t>(
        reinterpret_cast<const uchar*>(received.constData() + 9));
    const uint16_t vport = qFromBigEndian<uint16_t>(
        reinterpret_cast<const uchar*>(received.constData() + 11));
    return sourcePort == expectedSourceGamePort && vport == 40 && received.mid(13) == payload;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qputenv("SHADNET_P2P_RELAY_ADDRESS", "127.0.0.1");
    SharedState shared{};
    StunServer server(&shared);
    if (!server.Start(QHostAddress::LocalHost, 41314))
        return 1;

    QUdpSocket alice;
    QUdpSocket bob;
    QUdpSocket aliceGame;
    QUdpSocket bobGame;
    if (!alice.bind(QHostAddress::LocalHost, 0) || !bob.bind(QHostAddress::LocalHost, 0) ||
        !aliceGame.bind(QHostAddress::LocalHost, 0) || !bobGame.bind(QHostAddress::LocalHost, 0))
        return 2;

    const uint16_t AliceGamePort = aliceGame.localPort();
    const uint16_t BobGamePort = bobGame.localPort();
    {
        QWriteLocker lock(&shared.matching.udpLock);
        shared.matching.udpExt["alice"] = {"127.0.0.1", AliceGamePort};
        shared.matching.udpExt["bob"] = {"127.0.0.1", BobGamePort};
        shared.matching.bbscRelayPeers["alice"].insert("bob");
        shared.matching.bbscRelayPeers["bob"].insert("alice");
    }

    alice.writeDatagram(Register("alice"), QHostAddress::LocalHost, 41314);
    bob.writeDatagram(Register("bob"), QHostAddress::LocalHost, 41314);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    if (!CheckDirection(alice, BobGamePort, bob, AliceGamePort, "alice-to-bob"))
        return 3;
    if (!CheckDirection(bob, AliceGamePort, alice, BobGamePort, "bob-to-alice"))
        return 4;

    QByteArray raw;
    raw.append(static_cast<char>(0xff));
    raw.append(static_cast<char>(0x83));
    const uint16_t vport = qToBigEndian<uint16_t>(40);
    raw.append(reinterpret_cast<const char*>(&vport), 2);
    raw.append(reinterpret_cast<const char*>(&vport), 2);
    raw.append("native-p2p-frame");
    if (aliceGame.writeDatagram(raw, QHostAddress::LocalHost, 41314) != raw.size() ||
        !WaitForDatagram(bobGame, 1000))
        return 5;
    QByteArray relayed;
    relayed.resize(static_cast<int>(bobGame.pendingDatagramSize()));
    QHostAddress relaySource;
    uint16_t relaySourcePort = 0;
    bobGame.readDatagram(relayed.data(), relayed.size(), &relaySource, &relaySourcePort);
    if (relayed != raw || relaySource != QHostAddress::LocalHost || relaySourcePort != 41314)
        return 6;

    // Both directions, both framing generations, and both mapped-address messages.
    for (bool modern : {false, true}) {
        for (bool control : {false, true}) {
            for (bool reverse : {false, true}) {
                QByteArray packet = modern ? QByteArray::fromHex("ff83ffffffff")
                                           : QByteArray::fromHex("ffffffff");
                const int headerSize = packet.size();
                QByteArray payload(control ? 50 : 62, '\0');
                payload.replace(0, 4, "SHAD");
                payload[4] = control ? 0x10 : 0x07;
                payload[5] = 1;
                const QByteArray from = reverse ? "bob" : "alice";
                const QByteArray to = reverse ? "alice" : "bob";
                payload.replace(6, from.size(), from);
                payload.replace(22, to.size(), to);
                const int offset = control ? 42 : 46;
                payload.replace(offset, 6, QByteArray::fromHex("7c4d263f0e4a"));
                packet.append(payload);
                QByteArray expected = packet;
                expected.replace(headerSize + offset, 6,
                                 QByteArray::fromHex("7f000001a162")); // 127.0.0.1:41314
                QUdpSocket& source = reverse ? bobGame : aliceGame;
                QUdpSocket& target = reverse ? aliceGame : bobGame;
                source.writeDatagram(packet, QHostAddress::LocalHost, 41314);
                if (!WaitForDatagram(target, 1000))
                    return 7;
                QByteArray result(target.pendingDatagramSize(), '\0');
                target.readDatagram(result.data(), result.size());
                if (result != expected)
                    return 8;
            }
        }
    }
    qInfo() << "BBSC_GAME_AND_TRANSPARENT_P2P_RELAY_OK";
    return 0;
}
