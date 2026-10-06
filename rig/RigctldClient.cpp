#include <QTcpSocket>
#include <QTimer>

#include "RigctldClient.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.rig.rigctldclient");

#define CONNECT_TIMEOUT 3000
#define REPLY_TIMEOUT   2000

RigctldClient::RigctldClient(QObject *parent) :
    QObject(parent),
    socket(new QTcpSocket(this)),
    timer(new QTimer(this)),
    port(0),
    index(0),
    linesLeft(0),
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

    linesLeft = ( index < expected.size() ) ? qMax(1, expected.at(index)) : 1;
    socket->write((pending.at(index) + "\n").toLatin1());
    timer->start(REPLY_TIMEOUT);
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

    const int newline = buffer.indexOf('\n');

    if ( newline < 0 )
        return;

    timer->stop();

    // read all expected lines, or later replies get out of step
    while ( linesLeft > 0 )
    {
        const int end = buffer.indexOf('\n');

        if ( end < 0 )
        {
            timer->start(REPLY_TIMEOUT);
            return;
        }

        const QString line = QString::fromLatin1(buffer.left(end)).trimmed();
        buffer.remove(0, end + 1);

        /* RPRT alone means no value or an error; no further lines follow, even
           for a multi-line command, so fill the rest with empty values. */
        if ( line.startsWith(QStringLiteral("RPRT")) )
        {
            while ( linesLeft > 0 )
            {
                collected << QString();
                linesLeft--;
            }
            break;
        }

        collected << line;
        linesLeft--;
    }

    index++;
    sendNext();
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
