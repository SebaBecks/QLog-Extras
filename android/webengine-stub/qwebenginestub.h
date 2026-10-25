#ifndef QLOG_ANDROID_QWEBENGINESTUB_H
#define QLOG_ANDROID_QWEBENGINESTUB_H

// Qt WebEngine is not available on Android. These classes keep the WebEngine
// API used by the map and show the page in Android's own WebView (Qt WebView).

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QWidget>

class QQuickView;
class QWebChannel;
class QWebSocketServer;

class QWebEngineProfile
{
public:
    void setHttpUserAgent(const QString &) {}
};

class QWebEnginePage : public QObject
{
    Q_OBJECT

public:
    enum NavigationType
    {
        NavigationTypeLinkClicked,
        NavigationTypeTyped,
        NavigationTypeFormSubmitted,
        NavigationTypeBackForward,
        NavigationTypeReload,
        NavigationTypeOther,
        NavigationTypeRedirect
    };

    enum JavaScriptConsoleMessageLevel
    {
        InfoMessageLevel,
        WarningMessageLevel,
        ErrorMessageLevel
    };

    explicit QWebEnginePage(QObject *parent = nullptr) : QObject(parent) {}

    QWebEngineProfile *profile() { return &stubProfile; }
    // WebView has no Qt transport; the channel is served on a local WebSocket
    void setWebChannel(QWebChannel *channel);
    quint16 webChannelPort() const;
    void runJavaScript(const QString &script);
    // WebView cannot open qrc pages; the page is copied to a file first
    void load(const QUrl &url);

    QUrl pendingUrl;

signals:
    void loadFinished(bool ok);
    void urlRequested(const QUrl &url);
    void scriptRequested(const QString &script);

protected:
    virtual bool acceptNavigationRequest(const QUrl &, NavigationType, bool) { return true; }
    virtual void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &,
                                          int, const QString &) {}

private:
    QWebEngineProfile stubProfile;
    QPointer<QWebChannel> channel;
    QWebSocketServer *socketServer = nullptr;
};

class QWebEngineView : public QWidget
{
    Q_OBJECT

public:
    explicit QWebEngineView(QWidget *parent = nullptr);
    ~QWebEngineView() override;

    QWebEnginePage *page() const { return currentPage.data(); }
    void setUrl(const QUrl &) {}
    void setPage(QWebEnginePage *page);

signals:
    void loadFinished(bool ok);

private slots:
    void loadUrl(const QUrl &url);
    void runScript(const QString &script);
    void pageLoaded(bool ok);

private:
    QPointer<QWebEnginePage> currentPage;
    QQuickView *quickView;
};

#endif // QLOG_ANDROID_QWEBENGINESTUB_H
