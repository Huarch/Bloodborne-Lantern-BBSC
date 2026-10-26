// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDebug>
#include <QDateTime>
#include <QStringList>
#include <QtEndian>
#include "stream_extractor.h"
#include "stun_server.h"

// Legacy BBSC clients frame signaling as [src vport][dst vport]. Newer Lantern/shadPS4
// clients use the PS4 P2P datagram header [marker][flags][src vport][dst vport]. Keep both
// forms here so a server upgrade does not strand either client generation.
static constexpr uint16_t SIGNALING_VPORT_NBO = 0xFFFF;
static constexpr int LEGACY_VPORT_HEADER_SIZE = 4;
static constexpr int MODERN_VPORT_HEADER_SIZE = 6;
static constexpr uint8_t P2P_MARKER = 0xFF;
static constexpr uint8_t P2P_SIGNALING_FLAGS = 0x83; // valid, plain UDP datagram

enum class SignalingFrame { Legacy, Modern };

static QByteArray FrameSignaling(const QByteArray& payload, SignalingFrame frame) {
    QByteArray framed;
    framed.reserve((frame == SignalingFrame::Modern ? MODERN_VPORT_HEADER_SIZE
                                                     : LEGACY_VPORT_HEADER_SIZE) +
                   payload.size());
    if (frame == SignalingFrame::Modern) {
        framed.append(static_cast<char>(P2P_MARKER));
        framed.append(static_cast<char>(P2P_SIGNALING_FLAGS));
    }
    framed.append(reinterpret_cast<const char*>(&SIGNALING_VPORT_NBO), 2);
    framed.append(reinterpret_cast<const char*>(&SIGNALING_VPORT_NBO), 2);
    framed.append(payload);
    return framed;
}

static bool StripVportHeader(const QByteArray& data, QByteArray* payload,
                             SignalingFrame* frame) {
    int vportOffset = 0;
    if (data.size() >= MODERN_VPORT_HEADER_SIZE &&
        static_cast<uint8_t>(data[0]) == P2P_MARKER &&
        static_cast<uint8_t>(data[1]) == P2P_SIGNALING_FLAGS) {
        vportOffset = 2;
        *frame = SignalingFrame::Modern;
    } else if (data.size() >= LEGACY_VPORT_HEADER_SIZE) {
        *frame = SignalingFrame::Legacy;
    } else {
        return false;
    }

    uint16_t src_vp, dst_vp;
    memcpy(&src_vp, data.constData() + vportOffset, 2);
    memcpy(&dst_vp, data.constData() + vportOffset + 2, 2);
    if (src_vp != SIGNALING_VPORT_NBO || dst_vp != SIGNALING_VPORT_NBO)
        return false; // not a signaling packet
    *payload = data.mid(vportOffset + LEGACY_VPORT_HEADER_SIZE);
    return true;
}

StunServer::StunServer(SharedState* shared, QObject* parent)
    : QObject(parent), m_socket(new QUdpSocket(this)), m_shared(shared),
      m_multiplayer(shared, m_socket) {
    connect(m_socket, &QUdpSocket::readyRead, this, &StunServer::OnReadyRead);
}

bool StunServer::Start(const QHostAddress& addr, uint16_t port) {
    if (!m_socket->bind(addr, port)) {
        qCritical() << "STUN UDP bind failed on" << addr.toString() << ":" << port
                    << m_socket->errorString();
        return false;
    }
    qInfo().nospace().noquote() << "STUN UDP listener on: " << addr.toString() << ":" << port;
    qInfo() << "BBSC game-datagram relay enabled on the STUN listener";
    const QString advertised = qEnvironmentVariable("SHADNET_P2P_RELAY_ADDRESS").trimmed();
    if (!advertised.isEmpty()) {
        const QHostAddress publicAddress(advertised);
        if (publicAddress.protocol() != QAbstractSocket::IPv4Protocol) {
            qCritical() << "Invalid SHADNET_P2P_RELAY_ADDRESS:" << advertised;
            return false;
        }
        QWriteLocker lk(&m_shared->matching.udpLock);
        m_shared->matching.p2pRelayAddress = advertised;
        m_shared->matching.p2pRelayPort = port;
        qInfo() << "Transparent P2P/signaling relay advertised at" << advertised << ":" << port;
    }
    return true;
}

bool StunServer::HandlePeerRelay(const QByteArray& data, const QHostAddress& sender,
                                 uint16_t senderPort) {
    QString senderNpid;
    QString relayAddress;
    uint16_t relayPort = 0;
    QList<QPair<QString, uint16_t>> targets;
    QStringList firstPairs;
    {
        QWriteLocker lk(&m_shared->matching.udpLock);
        if (m_shared->matching.p2pRelayAddress.isEmpty())
            return false;
        relayAddress = m_shared->matching.p2pRelayAddress;
        relayPort = m_shared->matching.p2pRelayPort;
        for (auto it = m_shared->matching.udpExt.constBegin();
             it != m_shared->matching.udpExt.constEnd(); ++it) {
            if (it.value().second == senderPort && QHostAddress(it.value().first) == sender) {
                senderNpid = it.key();
                break;
            }
        }
        if (senderNpid.isEmpty())
            return false;
        QString addressedTarget;
        QByteArray signal;
        SignalingFrame signalFrame = SignalingFrame::Legacy;
        if (StripVportHeader(data, &signal, &signalFrame) && signal.startsWith("SHAD")) {
            const auto type = signal.size() > 4 ? static_cast<uint8_t>(signal[4]) : 0;
            const auto routeKey = [](const QString& name, quint32 id) {
                return name + QLatin1Char('|') + QString::number(id);
            };
            if ((type == 7 && signal.size() == 62) || (type == 16 && signal.size() == 50)) {
                const auto nameAt = [&](int offset) {
                    QByteArray name = signal.mid(offset, 16);
                    const int zero = name.indexOf('\0'); if (zero >= 0) name.truncate(zero);
                    return QString::fromLatin1(name);
                };
                if (nameAt(6) != senderNpid) return true;
                addressedTarget = nameAt(22);
                if (!m_shared->matching.bbscRelayPeers.value(senderNpid).contains(addressedTarget)) return true;
                const auto id = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(signal.constData()+38));
                if (m_signalRoutes.size() >= 4096) m_signalRoutes.clear();
                m_signalRoutes[routeKey(senderNpid, id)] = addressedTarget;
            } else if ((type == 5 || type == 6) && signal.size() == 17) {
                const auto id = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(signal.constData()+5));
                const auto stamp = signal.mid(9, 8).toHex();
                const auto now = QDateTime::currentMSecsSinceEpoch();
                for (auto it = m_echoReplies.begin(); it != m_echoReplies.end();) {
                    if (now - it->second > 10000) it = m_echoReplies.erase(it); else ++it;
                }
                if (type == 5) {
                    addressedTarget = m_signalRoutes.value(routeKey(senderNpid, id));
                    if (addressedTarget.isEmpty()) return true;
                    if (m_echoReplies.size() >= 8192) m_echoReplies.clear();
                    m_echoReplies[routeKey(addressedTarget, id) + QLatin1Char('|') + stamp] = {senderNpid, now};
                } else {
                    const auto key = routeKey(senderNpid, id) + QLatin1Char('|') + stamp;
                    addressedTarget = m_echoReplies.take(key).first;
                    if (addressedTarget.isEmpty()) return true;
                }
            }
        }
        for (const QString& peer : m_shared->matching.bbscRelayPeers.value(senderNpid)) {
            if (!addressedTarget.isEmpty() && peer != addressedTarget) continue;
            const auto endpoint = m_shared->matching.udpExt.constFind(peer);
            if (endpoint == m_shared->matching.udpExt.constEnd())
                continue;
            targets.append(endpoint.value());
            const QString pair = senderNpid + QStringLiteral("->") + peer;
            if (!m_shared->matching.p2pRelayActivePairs.contains(pair)) {
                m_shared->matching.p2pRelayActivePairs.insert(pair);
                firstPairs.append(pair);
            }
        }
    }
    QByteArray forwarded = data;
    QByteArray payload;
    SignalingFrame frame = SignalingFrame::Legacy;
    if (StripVportHeader(data, &payload, &frame) && payload.startsWith("SHAD")) {
        // Lantern prefers the embedded mapped address over the UDP source address.
        // Retaining a peer's public address would send its reply around the relay.
        int mappedOffset = -1;
        if (payload.size() == 62 && static_cast<uint8_t>(payload[4]) == 0x07)
            mappedOffset = 46; // SignalingHandshake
        else if (payload.size() == 50 && static_cast<uint8_t>(payload[4]) == 0x10)
            mappedOffset = 42; // SignalingControl
        if (mappedOffset >= 0) {
            const uint32_t address = qToBigEndian(QHostAddress(relayAddress).toIPv4Address());
            const uint16_t port = qToBigEndian(relayPort);
            const int headerSize = data.size() - payload.size();
            memcpy(forwarded.data() + headerSize + mappedOffset, &address, sizeof(address));
            memcpy(forwarded.data() + headerSize + mappedOffset + 4, &port, sizeof(port));
        }
    }
    for (const auto& target : targets)
        m_socket->writeDatagram(forwarded, QHostAddress(target.first), target.second);
    for (const QString& pair : firstPairs)
        qInfo() << "Transparent P2P relay forwarding:" << pair;
    return !targets.isEmpty();
}

bool StunServer::HandleBbscRelay(const QByteArray& data, const QHostAddress& sender,
                                 uint16_t senderPort) {
    // BBU1 01 id[16]                                  register/keepalive
    // BBU1 03 dst-ip[4] dst-port[2] vport[2] payload  mirrored game datagram
    // BBU1 04 src-ip[4] src-port[2] vport[2] payload  forwarded game datagram
    if (data.size() < 5 || data.left(4) != QByteArrayLiteral("BBU1"))
        return false;
    const uint8_t type = static_cast<uint8_t>(data[4]);
    QString senderIp = sender.toString();
    if (senderIp.startsWith("::ffff:"))
        senderIp = senderIp.mid(7);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (type == 0x01 && data.size() == 21) {
        const QByteArray rawId = data.mid(5, 16);
        const int zero = rawId.indexOf('\0');
        const QString npid = QString::fromUtf8(rawId.left(zero >= 0 ? zero : 16)).trimmed();
        if (npid.isEmpty())
            return true;
        QWriteLocker lk(&m_shared->matching.udpLock);
        const auto game = m_shared->matching.udpExt.constFind(npid);
        if (game == m_shared->matching.udpExt.constEnd() ||
            QHostAddress(game.value().first) != QHostAddress(senderIp)) {
            qWarning() << "BBSC relay registration rejected:" << npid << senderIp;
            return true;
        }
        const auto previous = m_shared->matching.bbscRelayExt.value(npid);
        m_shared->matching.bbscRelayExt[npid] = {senderIp, senderPort};
        m_shared->matching.bbscRelaySeenMs[npid] = now;
        if (previous.first != senderIp || previous.second != senderPort)
            qInfo() << "BBSC relay registered:" << npid << senderIp << ":" << senderPort;
        return true;
    }
    if (type != 0x03 || data.size() < 14)
        return true;

    const uint32_t dstIp = qFromBigEndian<uint32_t>(
        reinterpret_cast<const uchar*>(data.constData() + 5));
    const uint16_t dstPort = qFromBigEndian<uint16_t>(
        reinterpret_cast<const uchar*>(data.constData() + 9));
    QString senderNpid;
    QString targetNpid;
    QPair<QString, uint16_t> sourceGame;
    QPair<QString, uint16_t> targetRelay;
    bool firstForward = false;
    {
        QWriteLocker lk(&m_shared->matching.udpLock);
        for (auto it = m_shared->matching.bbscRelayExt.constBegin();
             it != m_shared->matching.bbscRelayExt.constEnd(); ++it) {
            if (it.value().second == senderPort && QHostAddress(it.value().first) == sender) {
                senderNpid = it.key();
                break;
            }
        }
        const QHostAddress wanted(dstIp);
        for (auto it = m_shared->matching.udpExt.constBegin();
             it != m_shared->matching.udpExt.constEnd(); ++it) {
            if (it.value().second == dstPort && QHostAddress(it.value().first) == wanted) {
                targetNpid = it.key();
                break;
            }
        }
        if (senderNpid.isEmpty() || targetNpid.isEmpty() ||
            !m_shared->matching.bbscRelayPeers.value(senderNpid).contains(targetNpid))
            return true;
        sourceGame = m_shared->matching.udpExt.value(senderNpid);
        targetRelay = m_shared->matching.bbscRelayExt.value(targetNpid);
        if (targetRelay.first.isEmpty() ||
            now - m_shared->matching.bbscRelaySeenMs.value(targetNpid) > 15000)
            return true;
        const QString pair = senderNpid + QStringLiteral("->") + targetNpid;
        if (!m_shared->matching.bbscRelayActivePairs.contains(pair)) {
            m_shared->matching.bbscRelayActivePairs.insert(pair);
            firstForward = true;
        }
    }

    const QHostAddress sourceAddress(sourceGame.first);
    if (sourceAddress.protocol() != QAbstractSocket::IPv4Protocol)
        return true;
    QByteArray forwarded("BBU1", 4);
    forwarded.append(static_cast<char>(0x04));
    const uint32_t sourceIpNbo = qToBigEndian(sourceAddress.toIPv4Address());
    const uint16_t sourcePortNbo = qToBigEndian(sourceGame.second);
    forwarded.append(reinterpret_cast<const char*>(&sourceIpNbo), 4);
    forwarded.append(reinterpret_cast<const char*>(&sourcePortNbo), 2);
    forwarded.append(data.constData() + 11, data.size() - 11); // vport + payload
    m_socket->writeDatagram(forwarded, QHostAddress(targetRelay.first), targetRelay.second);
    if (firstForward)
        qInfo() << "BBSC relay forwarding:" << senderNpid << "->" << targetNpid;
    return true;
}

void StunServer::OnReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray raw;
        raw.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        uint16_t senderPort = 0;
        m_socket->readDatagram(raw.data(), raw.size(), &sender, &senderPort);

        if (raw.isEmpty())
            continue;

        if (m_multiplayer.Handle(raw, sender, senderPort))
            continue;

        if (HandleBbscRelay(raw, sender, senderPort))
            continue;

        // Strip either the legacy BBSC header or the current PS4 P2P signaling header.
        QByteArray data;
        SignalingFrame frame = SignalingFrame::Legacy;
        if (!StripVportHeader(raw, &data, &frame) || data.isEmpty()) {
            HandlePeerRelay(raw, sender, senderPort);
            continue;
        }

        uint8_t cmd = static_cast<uint8_t>(data[0]);
        switch (cmd) {
        case 0x01:
            HandleStunPing(data, sender, senderPort, frame == SignalingFrame::Modern);
            break;
        default:
            HandlePeerRelay(raw, sender, senderPort);
            break;
        }
    }
}

void StunServer::HandleStunPing(const QByteArray& data, const QHostAddress& sender,
                                uint16_t senderPort, bool modernFrame) {
    // cmd(1) + online_id(16) + local_ip(4) = 21 bytes minimum
    if (data.size() < 21)
        return;

    // Extract null-padded online_id from bytes 1-16
    QByteArray rawId = data.mid(1, 16);
    int nullPos = rawId.indexOf('\0');
    QString npid = QString::fromUtf8(rawId.left(nullPos >= 0 ? nullPos : 16)).trimmed();
    if (npid.isEmpty())
        return;

    QString extIpStr = sender.toString();
    // Strip IPv6-mapped prefix if present (e.g. "::ffff:192.168.1.1" -> "192.168.1.1")
    if (extIpStr.startsWith("::ffff:"))
        extIpStr = extIpStr.mid(7);

    qInfo() << "STUN ping: npid=" << npid << "ext=" << extIpStr << ":" << senderPort;

    // Record external endpoint
    {
        QWriteLocker lk(&m_shared->matching.udpLock);
        m_shared->matching.udpExt[npid] = {extIpStr, senderPort};
    }

    // Reply: ext_ip(4 bytes network order) + ext_port(2 bytes network order)
    QHostAddress extAddr(extIpStr);
    uint32_t ipNet = qToBigEndian(static_cast<uint32_t>(extAddr.toIPv4Address()));
    uint16_t portNet = qToBigEndian(senderPort);

    QByteArray response;
    response.append(reinterpret_cast<const char*>(&ipNet), 4);
    response.append(reinterpret_cast<const char*>(&portNet), 2);
    m_socket->writeDatagram(
        FrameSignaling(response,
                       modernFrame ? SignalingFrame::Modern : SignalingFrame::Legacy),
        sender, senderPort);
}
