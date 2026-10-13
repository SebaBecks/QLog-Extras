#ifndef QLOG_ANDROID_ANDROIDSUPPORT_H
#define QLOG_ANDROID_ANDROIDSUPPORT_H

#include <QList>
#include <QObject>

class QApplication;
class QWindow;

// Android-only adjustments installed once at startup
class AndroidSupport : public QObject
{
public:
    // Must run before QApplication is created
    static void beforeApplication();
    static void install(QApplication *app);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    explicit AndroidSupport(QObject *parent) : QObject(parent) {}

    bool repost(QWindow *window, QEvent *original, QEvent *fixed);

    QList<QEvent *> reposted;
};

#endif // QLOG_ANDROID_ANDROIDSUPPORT_H
