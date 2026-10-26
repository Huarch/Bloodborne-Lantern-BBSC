// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHostAddress>
#include <QHash>
#include <QObject>
#include <QUdpSocket>
#include "client_session.h"
#include "multiplayer_relay.h"

class StunServer : public QObject {
    Q_OBJECT
public:
    explicit StunServer(SharedState* shared, QObject* parent = nullptr);
    bool Start(const QHostAddress& addr, uint16_t port);

private slots:
    void OnReadyRead();

private:
    void HandleStunPing(const QByteArray& data, const QHostAddress& sender, uint16_t senderPort,
                        bool modernFrame);
    bool HandleBbscRelay(const QByteArray& data, const QHostAddress& sender, uint16_t senderPort);
    bool HandlePeerRelay(const QByteArray& data, const QHostAddress& sender, uint16_t senderPort);

    QUdpSocket* m_socket;
    SharedState* m_shared;
    MultiplayerRelay m_multiplayer;
    QHash<QString, QString> m_signalRoutes;
    QHash<QString, QPair<QString, qint64>> m_echoReplies;
};
