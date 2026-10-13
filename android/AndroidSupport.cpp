#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QWindow>

#include "AndroidSupport.h"

void AndroidSupport::beforeApplication()
{
    // The native Android menu has no visible entry point; draw QLog's menu bar in the window
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
}

void AndroidSupport::install(QApplication *app)
{
    app->installEventFilter(new AndroidSupport(app));
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
    // Qt 6.8 on Android subtracts a window's position in logical instead of
    // device pixels, so taps in menus and dialogs land pos*(dpr-1)/dpr too far
    QWindow *window = reposted.contains(event) ? nullptr : qobject_cast<QWindow *>(obj);

    if ( window && window->position() != QPoint(0, 0) && window->devicePixelRatio() > 1.0 )
    {
        const qreal dpr = window->devicePixelRatio();
        const QPointF shift = QPointF(window->position()) * (dpr - 1.0) / dpr;

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
                dialog->setGeometry(available);
            }
        }
    }

    return QObject::eventFilter(obj, event);
}
