#include <QCoreApplication>
#include <QJniEnvironment>
#include <QPermissions>
#include <QRegularExpression>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "BluetoothSerialBridge.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.android.bluetoothserialbridge");

const QString BluetoothSerialBridge::PREFIX = QStringLiteral("bt:");

namespace
{
const char *SPP_UUID = "00001101-0000-1000-8000-00805F9B34FB";
const int BUFFER_SIZE = 512;

QJniObject defaultAdapter()
{
    return QJniObject::callStaticObjectMethod("android/bluetooth/BluetoothAdapter",
                                              "getDefaultAdapter",
                                              "()Landroid/bluetooth/BluetoothAdapter;");
}

// A pending Java exception must be cleared before the next JNI call
bool failed(QJniEnvironment &env)
{
    return env.checkAndClearExceptions();
}
}

bool BluetoothSerialBridge::isBluetoothPath(const QString &portPath)
{
    return portPath.startsWith(PREFIX);
}

QStringList BluetoothSerialBridge::pairedDevices()
{
    FCT_IDENTIFICATION;

    QStringList list;
    QJniEnvironment env;
    QJniObject adapter = defaultAdapter();

    if ( failed(env) || !adapter.isValid() )
        return list;

    QJniObject devices = adapter.callObjectMethod("getBondedDevices", "()Ljava/util/Set;");

    // no permission yet or Bluetooth is off
    if ( failed(env) || !devices.isValid() )
        return list;

    QJniObject array = devices.callObjectMethod("toArray", "()[Ljava/lang/Object;");
    jobjectArray objects = array.object<jobjectArray>();
    const jsize count = env->GetArrayLength(objects);

    for ( jsize i = 0; i < count; ++i )
    {
        QJniObject device = QJniObject::fromLocalRef(env->GetObjectArrayElement(objects, i));
        list << PREFIX + device.callObjectMethod<jstring>("getName").toString()
                + " " + device.callObjectMethod<jstring>("getAddress").toString();
    }

    failed(env);
    qCDebug(runtime) << list;
    return list;
}

void BluetoothSerialBridge::withPermission(QObject *context, const std::function<void()> &granted)
{
    FCT_IDENTIFICATION;

    QBluetoothPermission permission;
    permission.setCommunicationModes(QBluetoothPermission::Access);

    switch ( qApp->checkPermission(permission) )
    {
    case Qt::PermissionStatus::Granted:
        granted();
        break;
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(permission, context, [granted](const QPermission &result)
        {
            if ( result.status() == Qt::PermissionStatus::Granted )
                granted();
        });
        break;
    default:
        qCWarning(runtime) << "Bluetooth permission denied";
        break;
    }
}

BluetoothSerialBridge::~BluetoothSerialBridge()
{
    stop();
}

quint16 BluetoothSerialBridge::start(const QString &portPath, QString *error)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << portPath;

    stop();

    static const QRegularExpression addressRE("([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}");
    const QRegularExpressionMatch match = addressRE.match(portPath);

    if ( !match.hasMatch() )
    {
        *error = QObject::tr("No Bluetooth address in %1").arg(portPath);
        return 0;
    }

    const QString address = match.captured(0).toUpper();
    QJniEnvironment env;
    QJniObject adapter = defaultAdapter();
    QJniObject device = adapter.callObjectMethod("getRemoteDevice",
                                                 "(Ljava/lang/String;)Landroid/bluetooth/BluetoothDevice;",
                                                 QJniObject::fromString(address).object<jstring>());
    QJniObject uuid = QJniObject::callStaticObjectMethod("java/util/UUID", "fromString",
                                                         "(Ljava/lang/String;)Ljava/util/UUID;",
                                                         QJniObject::fromString(SPP_UUID).object<jstring>());

    if ( failed(env) || !device.isValid() || !uuid.isValid() )
    {
        *error = QObject::tr("Bluetooth device %1 not found").arg(address);
        return 0;
    }

    socket = device.callObjectMethod("createRfcommSocketToServiceRecord",
                                     "(Ljava/util/UUID;)Landroid/bluetooth/BluetoothSocket;",
                                     uuid.object());
    // discovery slows the connection down; may fail without the scan permission
    adapter.callMethod<jboolean>("cancelDiscovery");
    failed(env);

    if ( socket.isValid() )
        socket.callMethod<void>("connect");

    if ( failed(env) || !socket.isValid() )
    {
        qCWarning(runtime) << "Bluetooth connect failed" << address;
        socket = QJniObject();
        *error = QObject::tr("Cannot connect to %1 over Bluetooth. "
                             "Is the rig on, paired and its Bluetooth data port enabled?").arg(address);
        return 0;
    }

    input = socket.callObjectMethod("getInputStream", "()Ljava/io/InputStream;");
    output = socket.callObjectMethod("getOutputStream", "()Ljava/io/OutputStream;");

    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address4 = {};
    address4.sin_family = AF_INET;
    address4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address4);

    if ( failed(env) || listener < 0
         || ::bind(listener, reinterpret_cast<sockaddr *>(&address4), sizeof(address4)) < 0
         || ::listen(listener, 1) < 0
         || ::getsockname(listener, reinterpret_cast<sockaddr *>(&address4), &length) < 0 )
    {
        *error = QObject::tr("Cannot open the local port for Bluetooth");
        stop();
        return 0;
    }

    running = true;
    serverThread = std::thread(&BluetoothSerialBridge::serve, this);

    const quint16 port = ntohs(address4.sin_port);
    qCDebug(runtime) << "Bluetooth" << address << "on 127.0.0.1:" << port;
    return port;
}

void BluetoothSerialBridge::stop()
{
    FCT_IDENTIFICATION;

    running = false;

    // closing the Bluetooth socket ends the blocking read
    if ( socket.isValid() )
    {
        QJniEnvironment env;
        socket.callMethod<void>("close");
        failed(env);
    }

    const int connection = client;

    if ( connection >= 0 )
        ::shutdown(connection, SHUT_RDWR);

    if ( listener >= 0 )
        ::shutdown(listener, SHUT_RDWR);

    if ( serverThread.joinable() )
        serverThread.join();

    if ( listener >= 0 )
    {
        ::close(listener);
        listener = -1;
    }

    socket = QJniObject();
    input = QJniObject();
    output = QJniObject();
}

// Hamlib -> rig; one Hamlib connection at a time
void BluetoothSerialBridge::serve()
{
    std::thread fromRig(&BluetoothSerialBridge::pumpFromRig, this);
    QJniEnvironment env;
    char buffer[BUFFER_SIZE];

    while ( running )
    {
        const int connection = ::accept(listener, nullptr, nullptr);

        if ( connection < 0 )
            break;

        int on = 1;
        ::setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        client = connection;

        ssize_t size;

        while ( running && (size = ::recv(connection, buffer, sizeof(buffer), 0)) > 0 )
        {
            jbyteArray bytes = env->NewByteArray(size);
            env->SetByteArrayRegion(bytes, 0, size, reinterpret_cast<const jbyte *>(buffer));
            output.callMethod<void>("write", "([B)V", bytes);
            output.callMethod<void>("flush");
            env->DeleteLocalRef(bytes);

            if ( failed(env) )
            {
                qCWarning(runtime) << "Bluetooth write failed";
                running = false;
            }
        }

        client = -1;
        ::close(connection);
    }

    running = false;
    fromRig.join();
}

// rig -> Hamlib; data with no Hamlib connected is dropped
void BluetoothSerialBridge::pumpFromRig()
{
    QJniEnvironment env;
    jbyteArray bytes = env->NewByteArray(BUFFER_SIZE);
    jbyte buffer[BUFFER_SIZE];

    while ( running )
    {
        const jint size = input.callMethod<jint>("read", "([B)I", bytes);

        if ( failed(env) || size < 0 )
            break;

        env->GetByteArrayRegion(bytes, 0, size, buffer);
        const int connection = client;

        if ( connection >= 0 )
            ::send(connection, buffer, size, MSG_NOSIGNAL);
    }

    env->DeleteLocalRef(bytes);

    if ( running )
        qCWarning(runtime) << "Bluetooth connection lost";

    running = false;

    // wake serve() so the rig sees the connection drop
    const int connection = client;

    if ( connection >= 0 )
        ::shutdown(connection, SHUT_RDWR);

    if ( listener >= 0 )
        ::shutdown(listener, SHUT_RDWR);
}
