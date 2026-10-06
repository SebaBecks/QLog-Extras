#ifndef QLOG_RIG_RIGCTLDCLIENT_H
#define QLOG_RIG_RIGCTLDCLIENT_H

#include <QObject>
#include <QStringList>

class QTcpSocket;
class QTimer;

/* Asynchronous rigctld line-protocol client for values the Rig layer does
   not publish (meters). Strictly one command at a time, never blocking. */
class RigctldClient : public QObject
{
    Q_OBJECT

public:
    explicit RigctldClient(QObject *parent = nullptr);
    ~RigctldClient();

    void setEndpoint(const QString &hostName, quint16 portNumber);
    bool isBusy() const { return busy; }

    /* Values come back in command order, empty for a failed command.
       lineCounts: reply lines per command (default 1; get_vfo_info has 5). */
    void query(const QStringList &commands, const QList<int> &lineCounts = QList<int>());

signals:
    void results(const QStringList &values);
    void failed(const QString &reason);

private slots:
    void socketConnected();
    void socketReadyRead();
    void socketFailed();
    void requestTimedOut();

private:
    void sendNext();
    void finish();
    void abandon(const QString &reason);

    QTcpSocket *socket;
    QTimer *timer;
    QStringList pending;
    QList<int> expected;
    int linesLeft;
    QStringList collected;
    QByteArray buffer;
    QString host;
    quint16 port;
    int index;
    bool busy;
};

#endif // QLOG_RIG_RIGCTLDCLIENT_H
