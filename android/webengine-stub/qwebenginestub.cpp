#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJniEnvironment>
#include <QJniObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickView>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebChannelAbstractTransport>
#include <QWebSocket>
#include <QWebSocketServer>

#include "qwebenginestub.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.android.webengine");

namespace
{
// QWebChannel messages over a WebSocket, as in Qt's standalone example
class WebSocketTransport : public QWebChannelAbstractTransport
{
public:
    explicit WebSocketTransport(QWebSocket *socket)
        : QWebChannelAbstractTransport(socket), socket(socket)
    {
        connect(socket, &QWebSocket::textMessageReceived, this, [this](const QString &text)
        {
            const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
            if ( document.isObject() )
                emit messageReceived(document.object(), this);
        });
        connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
    }

    void sendMessage(const QJsonObject &message) override
    {
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }

private:
    QWebSocket *socket;
};

// An accessibility service walking the page (Lenovo's stylus service) stalls
// Android's WebView: pages stay at 10 %. The map has nothing to read anyway.
void hideWebViewsFromAccessibility()
{
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([]()
    {
        const jint NO_HIDE_DESCENDANTS = 4;
        QJniEnvironment env;
        jclass group = env.findClass("android/view/ViewGroup");
        jclass webView = env.findClass("android/webkit/WebView");
        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        QList<QJniObject> views { activity.callObjectMethod("getWindow", "()Landroid/view/Window;")
                                          .callObjectMethod("getDecorView", "()Landroid/view/View;") };

        while ( !views.isEmpty() )
        {
            QJniObject view = views.takeLast();

            if ( env->IsInstanceOf(view.object(), webView) )
                view.callMethod<void>("setImportantForAccessibility", "(I)V", NO_HIDE_DESCENDANTS);
            else if ( env->IsInstanceOf(view.object(), group) )
                for ( jint i = 0; i < view.callMethod<jint>("getChildCount"); ++i )
                    views << view.callObjectMethod("getChildAt", "(I)Landroid/view/View;", i);
        }

        env.checkAndClearExceptions();
    });
}
}

void QWebEnginePage::setWebChannel(QWebChannel *newChannel)
{
    FCT_IDENTIFICATION;

    channel = newChannel;

    if ( !channel || socketServer )
        return;

    socketServer = new QWebSocketServer(QStringLiteral("QLog map"), QWebSocketServer::NonSecureMode, this);

    if ( !socketServer->listen(QHostAddress::LocalHost) )
        qCWarning(runtime) << "Cannot open the map channel" << socketServer->errorString();

    connect(socketServer, &QWebSocketServer::newConnection, this, [this]()
    {
        while ( QWebSocket *socket = socketServer->nextPendingConnection() )
            if ( channel )
                channel->connectTo(new WebSocketTransport(socket));
    });
}

quint16 QWebEnginePage::webChannelPort() const
{
    return socketServer ? socketServer->serverPort() : 0;
}

void QWebEnginePage::runJavaScript(const QString &script)
{
    emit scriptRequested(script);
}

void QWebEnginePage::load(const QUrl &url)
{
    FCT_IDENTIFICATION;

    QFile file(QStringLiteral(":") + url.path());

    if ( !file.open(QIODevice::ReadOnly) )
    {
        qCWarning(runtime) << "Cannot read" << url;
        emit loadFinished(false);
        return;
    }

    // Qt WebView's loadHtml() shows percent-encoded text on Android, so the page goes through a file
    const QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/" + url.fileName();
    QFile copy(path);

    if ( !QDir().mkpath(QFileInfo(path).absolutePath())
         || !copy.open(QIODevice::WriteOnly | QIODevice::Truncate)
         || copy.write(file.readAll()) < 0 )
    {
        qCWarning(runtime) << "Cannot write" << path;
        emit loadFinished(false);
        return;
    }

    copy.close();

    // the query (e.g. language) must stay visible to the page script
    pendingUrl = QUrl::fromLocalFile(path);
    pendingUrl.setQuery(url.query());
    emit urlRequested(pendingUrl);
}

QWebEngineView::QWebEngineView(QWidget *parent)
    : QWidget(parent),
      quickView(new QQuickView())
{
    FCT_IDENTIFICATION;

    // Android's WebView is a native view; it needs a real window, not a QQuickWidget
    quickView->setResizeMode(QQuickView::SizeRootObjectToView);
    quickView->setSource(QUrl(QStringLiteral("qrc:/android/OnlineMapView.qml")));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(QWidget::createWindowContainer(quickView, this));

    if ( QQuickItem *webView = quickView->rootObject() )
        connect(webView, SIGNAL(pageLoaded(bool)), this, SLOT(pageLoaded(bool)));
    else
        qCWarning(runtime) << "Map view failed" << quickView->errors();
}

QWebEngineView::~QWebEngineView()
{
    // the container owns the window
}

void QWebEngineView::setPage(QWebEnginePage *page)
{
    FCT_IDENTIFICATION;

    if ( currentPage )
        disconnect(currentPage.data(), nullptr, this, nullptr);

    currentPage = page;

    if ( !page )
        return;

    connect(page, &QWebEnginePage::urlRequested, this, &QWebEngineView::loadUrl);
    connect(page, &QWebEnginePage::scriptRequested, this, &QWebEngineView::runScript);

    if ( !page->pendingUrl.isEmpty() )
        loadUrl(page->pendingUrl);
}

void QWebEngineView::loadUrl(const QUrl &url)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << url;

    // the native WebView appears a moment after the QML item
    hideWebViewsFromAccessibility();
    QTimer::singleShot(500, this, &hideWebViewsFromAccessibility);

    if ( QQuickItem *webView = quickView->rootObject() )
        webView->setProperty("target", url);
}

void QWebEngineView::runScript(const QString &script)
{
    if ( QQuickItem *webView = quickView->rootObject() )
        QMetaObject::invokeMethod(webView, "runJavaScript", Q_ARG(QString, script));
}

void QWebEngineView::pageLoaded(bool ok)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << ok;

    if ( currentPage )
        emit currentPage->loadFinished(ok);

    emit loadFinished(ok);
}
