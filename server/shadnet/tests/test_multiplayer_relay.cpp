#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QtEndian>
#include <QUdpSocket>
#include "stun_server.h"
#include "bbsc_multiplayer_queue.h"
#include <cstdlib>

static void Check(bool b, const char* message) { if (!b) { qCritical() << message; std::exit(1); } }
template<class T> static void Put(QByteArray& b, T n) { T v=qToBigEndian(n); b.append(reinterpret_cast<char*>(&v),sizeof(v)); }
template<class T> static T Read(const QByteArray& b,int at) { return qFromBigEndian<T>(reinterpret_cast<const uchar*>(b.constData()+at)); }
static void Pump(int ms=20) { QElapsedTimer t;t.start(); do { QCoreApplication::processEvents(); QThread::msleep(1); } while(t.elapsed()<ms); }
static QByteArray Receive(QUdpSocket& socket) {
    QElapsedTimer t;t.start(); while(!socket.hasPendingDatagrams() && t.elapsed()<500) Pump(1);
    Check(socket.hasPendingDatagrams(),"missing expected datagram");
    QByteArray b(socket.pendingDatagramSize(),'\0');socket.readDatagram(b.data(),b.size());return b;
}
static QByteArray Frame(char kind,quint64 epoch) { QByteArray b("BBU2",4);b.append(kind);Put(b,epoch);return b; }
static void Name(QByteArray& b,const QString& name) { auto n=name.toLatin1();n.resize(16,'\0');b.append(n); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); qputenv("SHADNET_P2P_RELAY_ADDRESS","127.0.0.1");
    SharedState shared; StunServer server(&shared); Check(server.Start(QHostAddress::LocalHost,41314),"bind");
    QUdpSocket clients[3], games[3]; QString names[3]={"alice","bob","carol"};
    quint32 aliases[3]{};quint64 epoch=0;
    auto send=[&](int who,const QByteArray& b) {clients[who].writeDatagram(b,QHostAddress::LocalHost,41314);};
    for(int i=0;i<3;++i) {
        Check(clients[i].bind(QHostAddress::LocalHost,0) && games[i].bind(QHostAddress::LocalHost,0),"client bind");
        shared.npidToUserId[names[i]]=i+1;shared.clients[i+1].npid=names[i];
        shared.matching.udpExt[names[i]]={"127.0.0.1",games[i].localPort()};
        QByteArray b("BBU2\1",5);Name(b,names[i]);send(i,b);auto ack=Receive(clients[i]);
        Check(ack.size()==19 && ack[4]==2,"registration reply");
        if(!epoch) epoch=Read<quint64>(ack,5);Check(epoch==Read<quint64>(ack,5),"epoch stable");aliases[i]=Read<quint32>(ack,13);
    }
    Check(aliases[0]!=aliases[1] && aliases[1]!=aliases[2] && aliases[0]!=aliases[2],"unique aliases behind same NAT");
    shared.matching.bbscRelayPeers["alice"].insert("bob");shared.matching.bbscRelayPeers["bob"].insert("alice");
    auto query=Frame(3,epoch);Name(query,"bob");send(0,query);auto response=Receive(clients[0]);Check(Read<quint32>(response,29)==aliases[1],"name resolution");
    auto data=[&](quint64 session,quint32 target) {auto b=Frame(7,session);Put(b,target);Put<quint16>(b,31314);Put<quint16>(b,40);Put<quint16>(b,30);b.append("game-payload");return b;};
    send(0,data(epoch,aliases[2]));Pump();Check(!clients[2].hasPendingDatagrams(),"unauthorized pair blocked");
    Room room;room.roomId=77;room.maxSlot=5;
    for(int i=0;i<3;++i) {RoomMember m;m.npid=names[i];m.userId=i+1;m.memberId=17+i*17;room.members[m.memberId]=m;}
    shared.matching.rooms[{"bloodborne",77}]=room;
    auto member=Frame(5,epoch);Put<quint64>(member,77);Put<quint16>(member,51);send(1,member);response=Receive(clients[1]);
    Check(response.size()==45 && Read<quint32>(response,39)==aliases[2],"room-member resolution");
    for(int from=0;from<3;++from) for(int to=0;to<3;++to) if(from!=to) {
        send(from,data(epoch,aliases[to]));response=Receive(clients[to]);
        Check(response[4]==8 && Read<quint32>(response,13)==aliases[from],"source identity preserved");
        Check(Read<quint16>(response,19)==40 && Read<quint16>(response,21)==30 && response.mid(23)=="game-payload","payload and vports intact");
        Pump();for(int other=0;other<3;++other) Check(!clients[other].hasPendingDatagrams(),"no sender echo or third-player leakage");
    }
    send(0,data(epoch^1,aliases[1]));Pump();Check(!clients[1].hasPendingDatagrams(),"old epoch rejected");
    auto malformed=data(epoch,aliases[1]);malformed.truncate(21);send(0,malformed);Pump();Check(!clients[1].hasPendingDatagrams(),"truncated frame rejected");
    // Native handshake and echo must also select one peer, even when A has two peers.
    shared.matching.bbscRelayPeers["alice"].insert("carol");shared.matching.bbscRelayPeers["carol"].insert("alice");
    auto signal=QByteArray::fromHex("ff83ffffffff");QByteArray payload(62,'\0');payload.replace(0,4,"SHAD");payload[4]=7;payload[5]=1;
    payload.replace(6,5,"alice");payload.replace(22,3,"bob");payload[38]=11;signal.append(payload);
    games[0].writeDatagram(signal,QHostAddress::LocalHost,41314);Receive(games[1]);Pump();Check(!games[2].hasPendingDatagrams(),"handshake not broadcast");
    QByteArray echo=QByteArray::fromHex("ff83ffffffff53484144050b0000000100000000000000");
    games[0].writeDatagram(echo,QHostAddress::LocalHost,41314);Check(Receive(games[1])==echo,"ping route");Pump();Check(!games[2].hasPendingDatagrams(),"ping not broadcast");
    echo[10]=6;games[1].writeDatagram(echo,QHostAddress::LocalHost,41314);Check(Receive(games[0])==echo,"pong route");
    // Room isolation overrides leftover signaling authorization.
    shared.matching.rooms[{"bloodborne",77}].members.remove(51);Room other;other.roomId=88;other.members[51]=room.members[51];shared.matching.rooms[{"bloodborne",88}]=other;
    send(0,data(epoch,aliases[2]));Pump();Check(!clients[2].hasPendingDatagrams(),"cross-room stale pair rejected");
    BbscAddressed::Inbox inbox;inbox.Bind(2,40);inbox.Bind(3,30);
    BbscAddressed::Packet p;p.destinationVport=30;p.sourceVport=40;p.received=100;p.data={1,2,3};inbox.Push(p);
    BbscAddressed::Packet out;Check(!inbox.Pop(2,101,false,out),"port 40 cannot steal port 30 data");
    Check(inbox.Pop(3,101,true,out) && inbox.Pop(3,101,false,out) && !inbox.Pop(3,101,false,out),"peek preserves packet");
    inbox.Push(p);inbox.Close(3);inbox.Bind(3,30);Check(!inbox.Pop(3,101,false,out),"reused socket has no stale data");
    inbox.Push(p);Check(!inbox.Pop(3,4000,false,out),"expired game packet rejected");
    for(int i=0;i<300;++i) inbox.Push(p);int count=0;while(inbox.Pop(3,101,false,out))++count;Check(count==256,"bounded receive queue");
    qInfo()<<"THREE_PLAYER_ADDRESSED_RELAY_AND_SOCKET_ISOLATION_OK";
}
