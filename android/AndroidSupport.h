#ifndef QLOG_ANDROID_ANDROIDSUPPORT_H
#define QLOG_ANDROID_ANDROIDSUPPORT_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QWindow>

class QApplication;

// Android-only adjustments installed once at startup
class AndroidSupport : public QObject
{
public:
    // Must run before QApplication is created
    static void beforeApplication();
    static void install(QApplication *app);
    // After the application style is set: borders wide enough for a finger
    static void applyTouchStyle(QApplication *app);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    explicit AndroidSupport(QObject *parent) : QObject(parent) {}

    static void addMissingRootCertificates();

    bool repost(QWindow *window, QEvent *original, QEvent *fixed);

    QList<QEvent *> reposted;
    QPointer<QWindow> pressWindow;
};

#endif // QLOG_ANDROID_ANDROIDSUPPORT_H
