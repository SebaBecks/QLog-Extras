#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QWindow>

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
}

void AndroidSupport::install(QApplication *app)
{
    app->installEventFilter(new AndroidSupport(app));
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

    return QObject::eventFilter(obj, event);
}
