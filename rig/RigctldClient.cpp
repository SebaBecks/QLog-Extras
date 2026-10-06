#include <QTcpSocket>
#include <QTimer>

#include "RigctldClient.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.rig.rigctldclient");

#define CONNECT_TIMEOUT 3000
// rigctld with the Icom scope stream answers slowly for a few seconds after start
#define REPLY_TIMEOUT   4000

RigctldClient::RigctldClient(QObject *parent) :
    QObject(parent),
    socket(new QTcpSocket(this)),
    timer(new QTimer(this)),
    port(0),
    index(0),
    busy(false)
{
    FCT_IDENTIFICATION;

    timer->setSingleShot(true);

    connect(socket, &QTcpSocket::connected, this, &RigctldClient::socketConnected);
    connect(socket, &QTcpSocket::readyRead, this, &RigctldClient::socketReadyRead);
    connect(socket, &QTcpSocket::errorOccurred, this, &RigctldClient::socketFailed);
    connect(timer, &QTimer::timeout, this, &RigctldClient::requestTimedOut);
}

RigctldClient::~RigctldClient()
{
    FCT_IDENTIFICATION;
    socket->abort();
}

void RigctldClient::setEndpoint(const QString &hostName, quint16 portNumber)
{
    FCT_IDENTIFICATION;

    if ( host == hostName && port == portNumber )
        return;

    host = hostName;
    port = portNumber;
    socket->abort();
}

void RigctldClient::query(const QStringList &commands, const QList<int> &lineCounts)
{
    FCT_IDENTIFICATION;

    if ( busy )
    {
        qCDebug(runtime) << "query while the previous one is still running - dropped";
        return;
    }

    if ( commands.isEmpty() || host.isEmpty() )
        return;

    busy = true;
    pending = commands;
    expected = lineCounts;
    collected.clear();
    buffer.clear();
    index = 0;

    if ( socket->state() == QAbstractSocket::ConnectedState )
    {
        sendNext();
        return;
    }

    // connect once; rigctld keeps the connection open
    socket->abort();
    socket->connectToHost(host, port);
    timer->start(CONNECT_TIMEOUT);
}

void RigctldClient::socketConnected()
{
    FCT_IDENTIFICATION;
    sendNext();
}

void RigctldClient::sendNext()
{
    FCT_IDENTIFICATION;

    if ( index >= pending.size() )
    {
        finish();
        return;
    }

    buffer.clear();
    replyLines.clear();

    /* Extended protocol: every reply ends with RPRT. Counting plain lines
       broke when a rig answered with more or fewer (IC-705 get_vfo_info
       adds RPRT -11), and every later reply went to the wrong command. */
    socket->write(("+" + pending.at(index) + "\n").toLatin1());
    timer->start(REPLY_TIMEOUT);
}

// "Frequency: 7140390" -> "7140390"; some replies carry the bare value
static QString replyValue(const QString &line)
{
    FCT_IDENTIFICATION;

    if ( line.endsWith(QChar(':')) )
        return QString();

    const int colon = line.indexOf(QStringLiteral(": "));

    return colon >= 0 ? line.mid(colon + 2).trimmed() : line;
}

void RigctldClient::socketReadyRead()
{
    FCT_IDENTIFICATION;

    buffer += socket->readAll();

    // unsolicited data
    if ( !busy || index >= pending.size() )
    {
        buffer.clear();
        return;
    }

    int end;

    while ( ( end = buffer.indexOf('\n') ) >= 0 )
    {
        const QString line = QString::fromLatin1(buffer.left(end)).trimmed();
        buffer.remove(0, end + 1);

        if ( !line.startsWith(QStringLiteral("RPRT")) )
        {
            replyLines << line;
            continue;
        }

        timer->stop();

        // first line echoes the command; an error leaves every value empty
        const bool ok = ( line == QStringLiteral("RPRT 0") );
        const int wanted = ( index < expected.size() ) ? qMax(1, expected.at(index)) : 1;

        for ( int i = 0; i < wanted; i++ )
            collected << ( ok ? replyValue(replyLines.value(i + 1)) : QString() );

        index++;
        sendNext();
        return;
    }
}

void RigctldClient::finish()
{
    FCT_IDENTIFICATION;

    timer->stop();
    busy = false;
    emit results(collected);
}

void RigctldClient::abandon(const QString &reason)
{
    FCT_IDENTIFICATION;

    qWarning() << "rigctld:" << reason;

    timer->stop();
    socket->abort();
    busy = false;
    emit failed(reason);
}

void RigctldClient::socketFailed()
{
    FCT_IDENTIFICATION;

    abandon(socket->errorString());
}

void RigctldClient::requestTimedOut()
{
    FCT_IDENTIFICATION;

    abandon(tr("rigctld did not answer in time"));
}
