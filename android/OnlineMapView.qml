import QtQuick
import QtWebView

// The online map in Android's WebView; driven from QWebEngineView (android/webengine-stub)
WebView {
    signal pageLoaded(bool ok)

    // the page is a file in QLog's cache directory
    settings.allowFileAccess: true
    settings.localContentCanAccessFileUrls: true

    onLoadingChanged: function(request) {
        if (request.status === WebView.LoadSucceededStatus)
            pageLoaded(true)
        else if (request.status === WebView.LoadFailedStatus)
            pageLoaded(false)
    }
}
