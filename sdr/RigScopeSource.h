#ifndef QLOG_SDR_RIGSCOPESOURCE_H
#define QLOG_SDR_RIGSCOPESOURCE_H

#include <QObject>
#include <QStringList>
#include <QVector>

class QTcpSocket;
class QTimer;
class QUdpSocket;

/* Rig's built-in scope (e.g. IC-705) as panadapter source, for rigs without IF out.
   Reads Hamlib >= 4.5 rigctld multicast JSON sweeps; rigctld needs
       -C async=1,multicast_data_addr=224.0.0.1,multicast_data_port=4535
   Turns the scope and its CI-V output on via rigctld, restores them on stop. */
class RigScopeSource : public QObject
{
    Q_OBJECT

public:
    explicit RigScopeSource(QObject *parent = nullptr);
    ~RigScopeSource() override;

    bool start(quint16 rigctld, const QString &multicastGroup, quint16 multicastPort);
    void stop();
    bool isRunning() const;
    // no sweep for SILENCE_MS
    bool isSilent() const { return silent; }
    QString lastError() const { return error; }
    // rig name and span, known after the first sweep
    QString description() const;
    /* Puts back a scope state left changed when rigctld went away before
       stop(); called by RigctldManager before it stops rigctld and after
       it starts. Blocks for at most a few seconds. */
    static void restorePending(quint16 rigctldPort);

signals:
    void lineReady(const QVector<float> &decibels, double firstHz, double binHz);
    /* Empty text = sweeps arriving again. */
    void statusChanged(const QString &text);

private slots:
    void datagramsReady();
    void controlConnected();
    void controlReadyRead();
    void checkSilence();

private:
    void parse(const QByteArray &datagram);
    void sendNextCommand();

    QUdpSocket *socket;
    QTcpSocket *control;
    QTimer *silenceTimer;
    QString error;
    QString group;
    quint16 port = 0;
    quint16 rigctldPort = 0;
    QString controlError;
    QString rigName;
    double spanHz = 0.0;
    qint64 lastLineAt = 0;
    bool silent = false;

    /* rigctld commands, sent one at a time. */
    QStringList commands;
    QByteArray controlBuffer;
    QString scopeBefore;
    QString outputBefore;
};

#endif // QLOG_SDR_RIGSCOPESOURCE_H
