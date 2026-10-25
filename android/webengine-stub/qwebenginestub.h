#ifndef QLOG_ANDROID_QWEBENGINESTUB_H
#define QLOG_ANDROID_QWEBENGINESTUB_H

// Qt WebEngine is not available on Android. These classes keep the map code
// compiling; the view shows a placeholder and scripts are ignored.

#include <QLabel>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

class QWebChannel;

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
    void setWebChannel(QWebChannel *) {}
    void runJavaScript(const QString &) {}

    // Report success so callers stop queueing scripts for a page that never loads
    void load(const QUrl &)
    {
        QTimer::singleShot(0, this, [this]() { emit loadFinished(true); });
    }

signals:
    void loadFinished(bool ok);

protected:
    virtual bool acceptNavigationRequest(const QUrl &, NavigationType, bool) { return true; }
    virtual void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &,
                                          int, const QString &) {}

private:
    QWebEngineProfile stubProfile;
};

class QWebEngineView : public QWidget
{
    Q_OBJECT

public:
    explicit QWebEngineView(QWidget *parent = nullptr) : QWidget(parent)
    {
        QLabel *label = new QLabel(tr("Map is not available on Android"), this);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->addWidget(label);
    }

    QWebEnginePage *page() const { return currentPage.data(); }
    void setUrl(const QUrl &) {}

    void setPage(QWebEnginePage *page)
    {
        if ( currentPage )
            disconnect(currentPage.data(), &QWebEnginePage::loadFinished,
                       this, &QWebEngineView::loadFinished);
        currentPage = page;
        if ( page )
            connect(page, &QWebEnginePage::loadFinished,
                    this, &QWebEngineView::loadFinished);
    }

signals:
    void loadFinished(bool ok);

private:
    QPointer<QWebEnginePage> currentPage;
};

#endif // QLOG_ANDROID_QWEBENGINESTUB_H
