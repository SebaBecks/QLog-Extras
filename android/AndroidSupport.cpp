#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QJniObject>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QScreen>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QStyle>
#include <QTimer>
#include <QWindow>
#include <QtWebView>

#include "AndroidSupport.h"

void AndroidSupport::beforeApplication()
{
    // The native Android menu has no visible entry point; draw QLog's menu bar in the window
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);

    // Without it Qt ignores ImhNoPredictiveText and keyboards keep composing words
    qputenv("QT_ANDROID_ENABLE_WORKAROUND_TO_DISABLE_PREDICTIVE_TEXT", "1");

    // Accessibility calls (e.g. Lenovo's stylus service) block Qt's main thread,
    // so new popups never get a surface and combo box lists stay invisible
    qputenv("QT_ANDROID_DISABLE_ACCESSIBILITY", "1");

    // the online map runs in Android's WebView
    QtWebView::initialize();

    applyScreenScale();
}

void AndroidSupport::applyScreenScale()
{
    if ( qEnvironmentVariableIsSet("QT_SCALE_FACTOR") )
        return;

    // QApplication does not exist yet, so read the display from Android directly
    const QJniObject metrics = QJniObject::callStaticObjectMethod("android/content/res/Resources", "getSystem",
                                                                  "()Landroid/content/res/Resources;")
                                   .callObjectMethod("getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
    if ( !metrics.isValid() )
        return;

    const int longSide = qMax(metrics.getField<jint>("widthPixels"), metrics.getField<jint>("heightPixels"));
    const float density = metrics.getField<jfloat>("density");
    if ( density <= 0 )
        return;

    // QLog's main window needs about 1280 logical pixels across; tablets such as
    // the Galaxy Tab S3 (2048 px at density 2) would get only 1024
    const qreal scale = qBound(0.75, longSide / (density * 1280.0), 1.0);
    if ( scale < 1.0 )
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scale, 'f', 3));
}

void AndroidSupport::install(QApplication *app)
{
    app->installEventFilter(new AndroidSupport(app));
    addMissingRootCertificates();
}

void AndroidSupport::addMissingRootCertificates()
{
    // Android 9 lacks GlobalSign Root R6, used by iota-world.org (IOTA list download)
    const QList<QSslCertificate> extra = QSslCertificate::fromPath(":/android/certs/globalsign-root-r6.pem");

    QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
    conf.setCaCertificates(QSslConfiguration::systemCaCertificates() + extra);
    QSslConfiguration::setDefaultConfiguration(conf);
}

namespace
{
// The borders between docks are a few pixels wide; a finger cannot drag them
class TouchStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                    const QWidget *widget = nullptr) const override
    {
        if ( metric == PM_DockWidgetSeparatorExtent || metric == PM_SplitterWidth )
            return 14;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
};
}

void AndroidSupport::applyTouchStyle(QApplication *app)
{
    // created by name: setStyle() deletes the current style, so it cannot be the base
    app->setStyle(new TouchStyle(app->style()->name()));
}

bool AndroidSupport::repost(QWindow *window, QEvent *original, QEvent *fixed)
{
    // A tap may open a modal dialog, so other events arrive while this one is in flight
    reposted.append(fixed);
    QCoreApplication::sendEvent(window, fixed);
    reposted.removeOne(fixed);
    // An unaccepted touch makes Qt synthesize mouse events from the original
    original->setAccepted(fixed->isAccepted());
    return true;
}

bool AndroidSupport::eventFilter(QObject *obj, QEvent *event)
{
    // Qt 6.8 on Android adds a window's logical position to device-pixel touch
    // coordinates, so taps in menus and dialogs land pos/dpr too far
    QWindow *window = reposted.contains(event) ? nullptr : qobject_cast<QWindow *>(obj);

    if ( window && (event->type() == QEvent::TouchBegin || event->type() == QEvent::MouseButtonPress) )
        pressWindow = window;

    // A touch that opens a popup (combo box list) ends in the popup window,
    // where Qt maps it correctly; shifting it would close the popup at once
    if ( window && window == pressWindow
         && window->position() != QPoint(0, 0) && window->devicePixelRatio() > 1.0 )
    {
        const qreal dpr = window->devicePixelRatio();
        const QPointF shift = QPointF(window->position()) / dpr;

        switch ( event->type() )
        {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        {
            const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
            QMouseEvent fixed(mouse->type(),
                              mouse->position() - shift,
                              mouse->scenePosition() - shift,
                              mouse->globalPosition() - shift,
                              mouse->button(), mouse->buttons(), mouse->modifiers(),
                              mouse->pointingDevice());
            fixed.setTimestamp(mouse->timestamp());
            return repost(window, event, &fixed);
        }
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        {
            const QTouchEvent *touch = static_cast<QTouchEvent *>(event);
            QList<QEventPoint> points;

            for ( const QEventPoint &point : touch->points() )
                points.append(QEventPoint(point.id(), point.state(),
                                          point.scenePosition() - shift,
                                          point.globalPosition() - shift));

            QTouchEvent fixed(touch->type(), touch->pointingDevice(), touch->modifiers(), points);
            fixed.setTimestamp(touch->timestamp());
            return repost(window, event, &fixed);
        }
        default:
            break;
        }
    }

    // Android keyboards compose whole words and Qt 6.8 merges them wrongly into
    // text already in the field (TEST1 -> TEST1TEST1we); type letter by letter
    if ( event->type() == QEvent::Polish )
    {
        QWidget *widget = qobject_cast<QWidget *>(obj);

        if ( widget && widget->testAttribute(Qt::WA_InputMethodEnabled) )
            widget->setInputMethodHints(widget->inputMethodHints() | Qt::ImhNoPredictiveText);
    }

    // While the keyboard is open, Qt 6.8 does not show partial repaints of
    // dialogs (typed text stays invisible); repaint the whole dialog instead
    switch ( event->type() )
    {
    case QEvent::KeyPress:
    case QEvent::InputMethod:
    case QEvent::FocusIn:
    case QEvent::MouseButtonRelease:
    {
        QWidget *widget = qobject_cast<QWidget *>(obj);
        QWidget *window = widget ? widget->window() : nullptr;

        if ( qobject_cast<QDialog *>(window) )
            QTimer::singleShot(0, window, [window]() { window->update(); });
        break;
    }
    default:
        break;
    }

    // A dialog taller than the screen loses its buttons and Android scales it,
    // which breaks touch again. Keep dialogs within the available area.
    if ( event->type() == QEvent::Show )
    {
        QDialog *dialog = qobject_cast<QDialog *>(obj);

        if ( dialog && dialog->isWindow() && dialog->screen() )
        {
            const QRect available = dialog->screen()->availableGeometry();

            if ( dialog->width() > available.width()
                 || dialog->height() > available.height() )
            {
                dialog->setMinimumSize(dialog->minimumSize().boundedTo(available.size()));
                dialog->setWindowState(dialog->windowState() | Qt::WindowMaximized);
            }
        }
    }

    // With QT_SCALE_FACTOR, Qt's Android plugin sizes maximized windows
    // without the factor, so they cover only part of the screen
    if ( event->type() == QEvent::Resize || event->type() == QEvent::WindowStateChange )
    {
        QWidget *widget = qobject_cast<QWidget *>(obj);

        if ( widget && widget->isWindow() && widget->isMaximized() && widget->screen() )
        {
            const QRect available = widget->screen()->availableGeometry();

            if ( widget->geometry() != available )
                QTimer::singleShot(0, widget, [widget, available]() { widget->setGeometry(available); });
        }
    }

    return QObject::eventFilter(obj, event);
}
