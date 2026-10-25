#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickView>
#include <QUrlQuery>
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

    if ( QQuickItem *webView = quickView->rootObject() )
        webView->setProperty("url", url);
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
