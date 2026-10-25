import QtQuick
import QtWebView

// The online map in Android's WebView; driven from QWebEngineView (android/webengine-stub)
WebView {
    id: web

    signal pageLoaded(bool ok)

    // Set by QWebEngineView. A load request can be lost while Android's UI
    // thread is busy, so it is repeated until loading starts.
    property url target
    property int attempts: 0
    property bool started: false

    // the page is a file in QLog's cache directory
    settings.allowFileAccess: true
    settings.localContentCanAccessFileUrls: true

    onTargetChanged: {
        attempts = 0
        started = false
        url = target
        retry.restart()
    }

    Timer {
        id: retry
        interval: 3000
        repeat: true
        onTriggered: {
            if (web.started || web.attempts >= 5) {
                stop()
                return
            }
            web.attempts++
            // a changed query makes it a new request
            web.url = web.target + "&retry=" + web.attempts
        }
    }

    onLoadingChanged: function(request) {
        started = true
        if (request.status === WebView.LoadSucceededStatus)
            pageLoaded(true)
        else if (request.status === WebView.LoadFailedStatus)
            pageLoaded(false)
    }
}
