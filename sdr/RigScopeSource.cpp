#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include "RigScopeSource.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.sdr.rigscopesource");

/* IC-705 sends ~4 sweeps/s. */
#define SILENCE_MS       5000
#define CONTROL_WAIT_MS  500

#define GET_SCOPE        "\\get_func SCOPE"
#define GET_OUTPUT       "\\get_func SPECTRUM"
#define SET_SCOPE        "\\set_func SCOPE %1"
#define SET_OUTPUT       "\\set_func SPECTRUM %1"

RigScopeSource::RigScopeSource(QObject *parent) :
    QObject(parent),
    socket(new QUdpSocket(this)),
    control(new QTcpSocket(this)),
    silenceTimer(new QTimer(this))
{
    FCT_IDENTIFICATION;

    connect(socket, &QUdpSocket::readyRead, this, &RigScopeSource::datagramsReady);
    connect(control, &QTcpSocket::connected, this, &RigScopeSource::controlConnected);
    connect(control, &QTcpSocket::readyRead, this, &RigScopeSource::controlReadyRead);
    connect(control, &QTcpSocket::errorOccurred, this, [this]()
    {
        controlError = control->errorString();
        qCDebug(runtime) << "rigctld:" << controlError;
    });
    connect(silenceTimer, &QTimer::timeout, this, &RigScopeSource::checkSilence);
}

RigScopeSource::~RigScopeSource()
{
    FCT_IDENTIFICATION;

    stop();
}

bool RigScopeSource::start(quint16 rigctld, const QString &multicastGroup, quint16 multicastPort)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << rigctld << multicastGroup << multicastPort;

    stop();

    const QHostAddress address(multicastGroup);

    if ( address.isNull() || !address.isMulticast() )
    {
        error = tr("%1 is not a multicast group address").arg(multicastGroup);
        return false;
    }

    if ( !socket->bind(QHostAddress::AnyIPv4, multicastPort,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint) )
    {
        error = tr("Cannot listen on port %1: %2").arg(multicastPort).arg(socket->errorString());
        return false;
    }

    if ( !socket->joinMulticastGroup(address) )
    {
        error = tr("Cannot join %1: %2").arg(multicastGroup, socket->errorString());
        socket->close();
        return false;
    }

    group = multicastGroup;
    port = multicastPort;
    rigctldPort = rigctld;
    rigName.clear();
    spanHz = 0.0;
    silent = false;
    lastLineAt = QDateTime::currentMSecsSinceEpoch();

    /* Read current scope state first so stop() can restore it. */
    scopeBefore.clear();
    outputBefore.clear();
    controlError.clear();
    controlBuffer.clear();
    commands.clear();
    commands << QStringLiteral(GET_SCOPE) << QStringLiteral(GET_OUTPUT)
             << QStringLiteral(SET_SCOPE).arg(1) << QStringLiteral(SET_OUTPUT).arg(1);
    control->connectToHost(QHostAddress::LocalHost, rigctldPort);

    silenceTimer->start(1000);
    return true;
}

void RigScopeSource::stop()
{
    FCT_IDENTIFICATION;

    silenceTimer->stop();
    commands.clear();

    if ( socket->state() == QAbstractSocket::BoundState )
        socket->close();

    /* Restore the scope; wait for rigctld so disconnecting does not cut the
       commands off. */
    if ( control->state() == QAbstractSocket::ConnectedState )
    {
        QStringList restore;

        if ( outputBefore == QLatin1String("0") || outputBefore == QLatin1String("1") )
            restore << QStringLiteral(SET_OUTPUT).arg(outputBefore);
        if ( scopeBefore == QLatin1String("0") || scopeBefore == QLatin1String("1") )
            restore << QStringLiteral(SET_SCOPE).arg(scopeBefore);

        for ( const QString &command : static_cast<const QStringList &>(restore) )
        {
            control->write(command.toLatin1() + '\n');
            control->waitForBytesWritten(CONTROL_WAIT_MS);
            control->waitForReadyRead(CONTROL_WAIT_MS);
            control->readAll();
        }

        control->disconnectFromHost();

        if ( control->state() != QAbstractSocket::UnconnectedState )
            control->waitForDisconnected(CONTROL_WAIT_MS);
    }

    control->abort();
    scopeBefore.clear();
    outputBefore.clear();
}

bool RigScopeSource::isRunning() const
{
    FCT_IDENTIFICATION;

    return socket->state() == QAbstractSocket::BoundState;
}

QString RigScopeSource::description() const
{
    FCT_IDENTIFICATION;

    if ( rigName.isEmpty() || spanHz <= 0 )
        return tr("Rig's scope");

    return tr("%1 scope - %2 kHz").arg(rigName).arg(spanHz / 1000.0, 0, 'f', 0);
}

void RigScopeSource::controlConnected()
{
    FCT_IDENTIFICATION;

    sendNextCommand();
}

void RigScopeSource::sendNextCommand()
{
    FCT_IDENTIFICATION;

    if ( commands.isEmpty() || control->state() != QAbstractSocket::ConnectedState )
        return;

    control->write(commands.first().toLatin1() + '\n');
}

void RigScopeSource::controlReadyRead()
{
    FCT_IDENTIFICATION;

    controlBuffer += control->readAll();

    int end;

    while ( ( end = controlBuffer.indexOf('\n') ) >= 0 )
    {
        const QString line = QString::fromLatin1(controlBuffer.left(end)).trimmed();
        controlBuffer.remove(0, end + 1);

        if ( commands.isEmpty() )
            continue;

        const QString command = commands.takeFirst();
        qCDebug(runtime) << command << "->" << line;

        // get: value; set: RPRT 0, or RPRT <code> on refusal
        if ( command == QLatin1String(GET_SCOPE) )
            scopeBefore = line;
        else if ( command == QLatin1String(GET_OUTPUT) )
            outputBefore = line;
        else if ( line != QLatin1String("RPRT 0") )
            controlError = tr("rigctld refused \"%1\": %2").arg(command, line);

        sendNextCommand();
    }
}

void RigScopeSource::datagramsReady()
{
    FCT_IDENTIFICATION;

    while ( socket->hasPendingDatagrams() )
    {
        QByteArray datagram;
        datagram.resize(int(socket->pendingDatagramSize()));
        socket->readDatagram(datagram.data(), datagram.size());
        parse(datagram);
    }
}

/* {"rig":{"name":"IC-705",...}, "spectra":[{"lowFreq":..., "highFreq":...,
   "minLevel":0, "maxLevel":160, "minStrength":-80, "maxStrength":0,
   "length":475, "data":"<2 hex digits per point>"}]}.
   State-only datagrams have no spectra. */
void RigScopeSource::parse(const QByteArray &datagram)
{
    FCT_IDENTIFICATION;

    const QJsonObject root = QJsonDocument::fromJson(datagram).object();
    const QJsonArray spectra = root.value(QStringLiteral("spectra")).toArray();

    if ( spectra.isEmpty() )
        return;

    // main scope first; dual-scope rigs add the sub one
    const QJsonObject line = spectra.at(0).toObject();
    const QByteArray levels = QByteArray::fromHex(line.value(QStringLiteral("data")).toString().toLatin1());
    const double low = line.value(QStringLiteral("lowFreq")).toDouble();
    const double high = line.value(QStringLiteral("highFreq")).toDouble();
    const double minLevel = line.value(QStringLiteral("minLevel")).toDouble();
    const double maxLevel = line.value(QStringLiteral("maxLevel")).toDouble();
    const double minStrength = line.value(QStringLiteral("minStrength")).toDouble();
    const double maxStrength = line.value(QStringLiteral("maxStrength")).toDouble();

    if ( levels.isEmpty() || high <= low || maxLevel <= minLevel || maxStrength <= minStrength )
        return;

    // minLevel..maxLevel maps to minStrength..maxStrength dB
    const double scale = ( maxStrength - minStrength ) / ( maxLevel - minLevel );
    QVector<float> decibels(levels.size());

    for ( int i = 0; i < levels.size(); i++ )
        decibels[i] = float(minStrength + ( quint8(levels.at(i)) - minLevel ) * scale);

    rigName = root.value(QStringLiteral("rig")).toObject().value(QStringLiteral("name")).toString();
    spanHz = high - low;
    lastLineAt = QDateTime::currentMSecsSinceEpoch();

    if ( silent )
    {
        silent = false;
        emit statusChanged(QString());
    }

    // points are bin centres
    const double binHz = spanHz / levels.size();
    emit lineReady(decibels, low + binHz / 2.0, binHz);
}

void RigScopeSource::checkSilence()
{
    FCT_IDENTIFICATION;

    if ( silent || QDateTime::currentMSecsSinceEpoch() - lastLineAt < SILENCE_MS )
        return;

    silent = true;

    QString text = tr("No scope data from the rig. Its Hamlib profile needs Share Rig via port, "
                      "with -C async=1,multicast_data_addr=%1,multicast_data_port=%2 as additional "
                      "arguments; an IC-705 also its CI-V USB Port unlinked from [REMOTE].")
                   .arg(group).arg(port);

    if ( !controlError.isEmpty() )
        text += QStringLiteral(" (") + controlError + QStringLiteral(")");

    emit statusChanged(text);
}
