#include "notification/DesktopNotification.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.notification.android.desktopnotification");

// No native notifications on Android yet; alerts stay inside QLog

void DesktopNotification::initializePlatform()
{
    FCT_IDENTIFICATION;

    platformContext = nullptr;
}

void DesktopNotification::shutdownPlatform()
{
    FCT_IDENTIFICATION;
}

bool DesktopNotification::showPlatform(DesktopNotificationCategory category,
                                       const QString &title,
                                       const QString &body,
                                       bool soundEnabled)
{
    FCT_IDENTIFICATION;

    Q_UNUSED(category)
    Q_UNUSED(title)
    Q_UNUSED(body)
    Q_UNUSED(soundEnabled)

    return false;
}
