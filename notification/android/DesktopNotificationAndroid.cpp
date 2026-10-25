#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/private/qjnihelpers_p.h>

#include "notification/DesktopNotification.h"
#include "core/debug.h"

MODULE_IDENTIFICATION("qlog.notification.android.desktopnotification");

// the Java string lives until the end of the full expression it is used in
#define JSTR(text) QJniObject::fromString(text).object<jstring>()

namespace
{
const char *CHANNEL_SOUND = "qlog_alerts";
const char *CHANNEL_SILENT = "qlog_alerts_silent";
const char *EXTRA_CATEGORY = "qlog.notification.category";

// A tap on a notification brings the activity back with an extra naming the category
class NotificationIntentListener : public QtAndroidPrivate::NewIntentListener
{
public:
    explicit NotificationIntentListener(DesktopNotification *owner) : owner(owner) {}

    bool handleNewIntent(JNIEnv *, jobject intent) override
    {
        QJniObject newIntent(intent);
        if ( !newIntent.callMethod<jboolean>("hasExtra", "(Ljava/lang/String;)Z", JSTR(EXTRA_CATEGORY)) )
            return false;

        QMetaObject::invokeMethod(owner, "activate", Qt::QueuedConnection,
                                  Q_ARG(DesktopNotificationCategory, DesktopNotificationCategory::ALERT));
        return true;
    }

private:
    DesktopNotification *owner;
};

struct AndroidNotificationContext
{
    explicit AndroidNotificationContext(DesktopNotification *owner) : listener(owner) {}

    NotificationIntentListener listener;
    jint nextId = 1;
};

QJniObject notificationManager(const QJniObject &context)
{
    return context.callObjectMethod("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
                                    JSTR(QStringLiteral("notification")));
}

void createChannel(const QJniObject &manager, const char *id, const QString &name, jint importance)
{
    QJniObject channel("android/app/NotificationChannel",
                       "(Ljava/lang/String;Ljava/lang/CharSequence;I)V",
                       JSTR(id), JSTR(name), importance);
    manager.callMethod<void>("createNotificationChannel", "(Landroid/app/NotificationChannel;)V", channel.object());
}

// Android 13 and newer ask the user before an app may post notifications
bool hasPermission(const QJniObject &context)
{
    if ( QtAndroidPrivate::androidSdkVersion() < 33 )
        return true;

    const QString permission = QStringLiteral("android.permission.POST_NOTIFICATIONS");

    if ( context.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I", JSTR(permission)) == 0 )
        return true;

    QJniEnvironment env;
    jobjectArray permissions = env->NewObjectArray(1, env.findClass("java/lang/String"), JSTR(permission));
    context.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", permissions, jint(1501));
    env->DeleteLocalRef(permissions);
    return false;
}
}

void DesktopNotification::initializePlatform()
{
    FCT_IDENTIFICATION;

    AndroidNotificationContext *context = new AndroidNotificationContext(this);
    platformContext = context;
    QtAndroidPrivate::registerNewIntentListener(&context->listener);

    QJniObject appContext = QNativeInterface::QAndroidApplication::context();
    QJniObject manager = notificationManager(appContext);
    const jint IMPORTANCE_LOW = 2;
    const jint IMPORTANCE_DEFAULT = 3;

    // a channel's sound cannot change later, hence one with and one without
    createChannel(manager, CHANNEL_SOUND, tr("QLog alerts"), IMPORTANCE_DEFAULT);
    createChannel(manager, CHANNEL_SILENT, tr("QLog alerts (silent)"), IMPORTANCE_LOW);

    QJniEnvironment env;
    if ( env.checkAndClearExceptions() )
        qCWarning(runtime) << "Cannot create the notification channels";
}

void DesktopNotification::shutdownPlatform()
{
    FCT_IDENTIFICATION;

    AndroidNotificationContext *context = static_cast<AndroidNotificationContext *>(platformContext);

    if ( context )
        QtAndroidPrivate::unregisterNewIntentListener(&context->listener);

    delete context;
    platformContext = nullptr;
}

bool DesktopNotification::showPlatform(DesktopNotificationCategory category,
                                       const QString &title,
                                       const QString &body,
                                       bool soundEnabled)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << title << body << soundEnabled;

    AndroidNotificationContext *notificationContext = static_cast<AndroidNotificationContext *>(platformContext);
    QJniObject context = QNativeInterface::QAndroidApplication::context();

    if ( !notificationContext || !hasPermission(context) )
        return false;

    QJniEnvironment env;

    // reopen QLog's activity; singleTop delivers the tap to NotificationIntentListener
    QJniObject activityClass = context.callObjectMethod("getClass", "()Ljava/lang/Class;");
    QJniObject intent("android/content/Intent", "(Landroid/content/Context;Ljava/lang/Class;)V",
                      context.object(), activityClass.object());
    const jint FLAG_ACTIVITY_SINGLE_TOP = 0x20000000;
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", FLAG_ACTIVITY_SINGLE_TOP);
    intent.callObjectMethod("putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
                            JSTR(EXTRA_CATEGORY), JSTR(categoryIdentifier(category)));

    const jint FLAG_UPDATE_CURRENT = 0x08000000;
    const jint FLAG_IMMUTABLE = 0x04000000;
    QJniObject pendingIntent = QJniObject::callStaticObjectMethod(
        "android/app/PendingIntent", "getActivity",
        "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;",
        context.object(), jint(0), intent.object(), FLAG_UPDATE_CURRENT | FLAG_IMMUTABLE);

    QJniObject resources = context.callObjectMethod("getResources", "()Landroid/content/res/Resources;");
    const jint icon = resources.callMethod<jint>("getIdentifier",
                                                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
                                                 JSTR(QStringLiteral("ic_notification")),
                                                 JSTR(QStringLiteral("drawable")),
                                                 context.callObjectMethod<jstring>("getPackageName").object<jstring>());

    QJniObject bigText("android/app/Notification$BigTextStyle", "()V");
    bigText.callObjectMethod("bigText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;", JSTR(body));

    const char *const builderSig = "Landroid/app/Notification$Builder;";
    QJniObject builder("android/app/Notification$Builder", "(Landroid/content/Context;Ljava/lang/String;)V",
                       context.object(), JSTR(soundEnabled ? CHANNEL_SOUND : CHANNEL_SILENT));
    builder.callObjectMethod("setSmallIcon", QByteArray("(I)").append(builderSig), icon);
    builder.callObjectMethod("setContentTitle", QByteArray("(Ljava/lang/CharSequence;)").append(builderSig), JSTR(title));
    builder.callObjectMethod("setContentText", QByteArray("(Ljava/lang/CharSequence;)").append(builderSig), JSTR(body));
    builder.callObjectMethod("setStyle", QByteArray("(Landroid/app/Notification$Style;)").append(builderSig), bigText.object());
    builder.callObjectMethod("setContentIntent", QByteArray("(Landroid/app/PendingIntent;)").append(builderSig), pendingIntent.object());
    builder.callObjectMethod("setAutoCancel", QByteArray("(Z)").append(builderSig), jboolean(true));

    QJniObject notification = builder.callObjectMethod("build", "()Landroid/app/Notification;");
    notificationManager(context).callMethod<void>("notify", "(ILandroid/app/Notification;)V",
                                                  notificationContext->nextId++, notification.object());

    if ( env.checkAndClearExceptions() )
    {
        qCWarning(runtime) << "Cannot post the notification";
        return false;
    }

    return true;
}
