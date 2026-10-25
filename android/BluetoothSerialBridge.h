#ifndef QLOG_ANDROID_BLUETOOTHSERIALBRIDGE_H
#define QLOG_ANDROID_BLUETOOTHSERIALBRIDGE_H

#include <QJniObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <thread>

class QObject;

// A Bluetooth serial port (SPP, e.g. IC-705) offered on a local TCP port,
// because Hamlib can open any rig as host:port but not an Android socket
class BluetoothSerialBridge
{
public:
    static const QString PREFIX;
    static bool isBluetoothPath(const QString &portPath);
    // "bt:<name> <address>" for every paired device
    static QStringList pairedDevices();
    // Main thread only; runs granted() once Bluetooth may be used
    static void withPermission(QObject *context, const std::function<void()> &granted);

    BluetoothSerialBridge() = default;
    ~BluetoothSerialBridge();

    // Blocks until the rig accepts the connection; returns the TCP port or 0
    quint16 start(const QString &portPath, QString *error);
    void stop();

private:
    void serve();
    void pumpFromRig();

    QJniObject socket;
    QJniObject input;
    QJniObject output;
    int listener = -1;
    std::atomic<int> client{-1};
    std::atomic<bool> running{false};
    std::thread serverThread;
};

#endif // QLOG_ANDROID_BLUETOOTHSERIALBRIDGE_H
