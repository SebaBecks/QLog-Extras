#ifndef QLOG_RIG_OMNIRIGCLIENT_H
#define QLOG_RIG_OMNIRIGCLIENT_H

#include <QObject>
#include <QByteArray>

class OmniRigReplySink;

/* Second OmniRig client beside QLog's driver: reads both VFOs and sends
   custom CAT commands (meters), answered asynchronously via customReply().
   Uses IDispatch by name, so it serves OmniRig 1 and V2. Windows only. */
class OmniRigClient : public QObject
{
    Q_OBJECT

public:
    explicit OmniRigClient(QObject *parent = nullptr);
    ~OmniRigClient();

    // version is 1 for OmniRig, 2 for Omni-Rig V2; rigNumber counts from 1
    bool open(int version, int rigNumber);
    void close();
    bool isOpen() const;

    // rig file name loaded by OmniRig, e.g. FTDX101D
    QString rigType();

    // OmniRig 1: <exe dir>/Rigs, V2: %APPDATA%/Afreet/Rigs; empty if not found
    QString rigFile();

    // Hz; false while the rig is off-line, 0 for a VFO the rig file does not read
    bool readVfos(qint64 &freqA, qint64 &freqB);

    // reply ends after replyLength bytes, or at replyEnd when that is 0
    bool sendCustomCommand(const QByteArray &command, int replyLength,
                           const QByteArray &replyEnd);

signals:
    void customReply(const QByteArray &command, const QByteArray &reply);

private:
    friend class OmniRigReplySink;
    void deliverReply(int rigNumber, const QByteArray &command, const QByteArray &reply);

    void *omniRig;
    void *rig;
    void *connectionPoint;
    OmniRigReplySink *sink;
    unsigned long cookie;
    int openRigNumber;
    int openVersion;
    bool comInitialized;
};

#endif // QLOG_RIG_OMNIRIGCLIENT_H
