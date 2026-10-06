#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QUuid>

#include "OmniRigClient.h"
#include "core/debug.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <ocidl.h>
#endif

MODULE_IDENTIFICATION("qlog.rig.omnirigclient");

#ifdef Q_OS_WIN

namespace
{

// the classes OmniRig 1 and Omni-Rig V2 register their OmniRigX object under
const QUuid OMNIRIG_V1_CLSID(QStringLiteral("{0839E8C6-ED30-4950-8087-966F970F0CAE}"));
const QUuid OMNIRIG_V2_CLSID(QStringLiteral("{74E87CF5-F8CE-4C38-82AE-1673839DB15F}"));

// and the event interfaces each of them fires
const QUuid OMNIRIG_V1_EVENTS(QStringLiteral("{2219175F-E561-47E7-AD17-73C4D8891AA1}"));
const QUuid OMNIRIG_V2_EVENTS(QStringLiteral("{9262D30B-EB2F-44C7-9F0A-847C7BBBB451}"));

// OmniRig's ST_ONLINE: the port is open and the rig answers
const long OMNIRIG_ONLINE = 4;

// CustomReply(long RigNumber, VARIANT Command, VARIANT Reply)
const DISPID CUSTOM_REPLY_DISPID = 5;

bool getProperty(IDispatch *object, const wchar_t *name, VARIANT &result)
{
    FCT_IDENTIFICATION;

    VariantInit(&result);

    if ( !object )
        return false;

    DISPID id = 0;
    LPOLESTR member = const_cast<LPOLESTR>(name);

    if ( FAILED(object->GetIDsOfNames(IID_NULL, &member, 1, LOCALE_USER_DEFAULT, &id)) )
        return false;

    DISPPARAMS none = { nullptr, nullptr, 0, 0 };

    return SUCCEEDED(object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                                    &none, &result, nullptr, nullptr));
}

bool getLong(IDispatch *object, const wchar_t *name, long &value)
{
    FCT_IDENTIFICATION;

    VARIANT result;

    if ( !getProperty(object, name, result) )
        return false;

    const bool ok = SUCCEEDED(VariantChangeType(&result, &result, 0, VT_I4));

    if ( ok )
        value = result.lVal;

    VariantClear(&result);
    return ok;
}

// event arguments may come wrapped in a reference
const VARIANT &unwrap(const VARIANT &value)
{
    FCT_IDENTIFICATION;

    if ( value.vt == ( VT_BYREF | VT_VARIANT ) && value.pvarVal )
        return *value.pvarVal;

    return value;
}

/* OmniRig passes commands and replies as byte arrays, or as strings when a
   client sent a string. */
QByteArray variantBytes(const VARIANT &wrapped)
{
    FCT_IDENTIFICATION;

    const VARIANT &value = unwrap(wrapped);

    if ( value.vt == VT_BSTR )
        return QString::fromWCharArray(value.bstrVal, SysStringLen(value.bstrVal)).toLatin1();

    if ( !( value.vt & VT_ARRAY ) )
        return QByteArray();

    SAFEARRAY *array = ( value.vt & VT_BYREF ) ? ( value.pparray ? *value.pparray : nullptr )
                                               : value.parray;
    LONG lower = 0;
    LONG upper = -1;

    if ( !array
         || FAILED(SafeArrayGetLBound(array, 1, &lower))
         || FAILED(SafeArrayGetUBound(array, 1, &upper)) )
        return QByteArray();

    QByteArray bytes;
    const VARTYPE type = value.vt & VT_TYPEMASK;

    for ( LONG i = lower; i <= upper; ++i )
    {
        if ( type == VT_UI1 )
        {
            unsigned char byte = 0;
            SafeArrayGetElement(array, &i, &byte);
            bytes.append(static_cast<char>(byte));
        }
        else if ( type == VT_VARIANT )
        {
            VARIANT element;
            VariantInit(&element);
            SafeArrayGetElement(array, &i, &element);

            if ( SUCCEEDED(VariantChangeType(&element, &element, 0, VT_UI1)) )
                bytes.append(static_cast<char>(element.bVal));

            VariantClear(&element);
        }
    }

    return bytes;
}

long variantLong(const VARIANT &wrapped)
{
    FCT_IDENTIFICATION;

    VARIANT copy;
    VariantInit(&copy);

    long value = 0;

    if ( SUCCEEDED(VariantChangeType(&copy, const_cast<VARIANT *>(&unwrap(wrapped)), 0, VT_I4)) )
        value = copy.lVal;

    VariantClear(&copy);
    return value;
}

// a byte array VARIANT the caller must clear
VARIANT byteArrayVariant(const QByteArray &bytes)
{
    FCT_IDENTIFICATION;

    VARIANT value;
    VariantInit(&value);

    SAFEARRAY *array = SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(bytes.size()));

    if ( !array )
        return value;

    void *data = nullptr;

    if ( SUCCEEDED(SafeArrayAccessData(array, &data)) )
    {
        memcpy(data, bytes.constData(), static_cast<size_t>(bytes.size()));
        SafeArrayUnaccessData(array);
    }

    value.vt = VT_ARRAY | VT_UI1;
    value.parray = array;
    return value;
}

}

/* Receives OmniRig's events. Only the answer to a custom command matters
   here; QLog's driver has a sink of its own for everything else. */
class OmniRigReplySink : public IDispatch
{
public:
    OmniRigReplySink(OmniRigClient *client, const IID &events) :
        refCount(1),
        owner(client),
        eventsIid(events)
    {}

    // the client goes before OmniRig lets go of the sink
    void detach() { owner = nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if ( !object )
            return E_POINTER;

        if ( riid == IID_IUnknown || riid == IID_IDispatch || riid == eventsIid )
        {
            *object = static_cast<IDispatch *>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG count = InterlockedDecrement(&refCount);

        if ( count == 0 )
            delete this;

        return count;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *count) override
    {
        if ( count )
            *count = 0;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *params,
                                     VARIANT *, EXCEPINFO *, UINT *) override
    {
        // the arguments arrive last first
        if ( id == CUSTOM_REPLY_DISPID && owner && params && params->cArgs == 3 )
            owner->deliverReply(static_cast<int>(variantLong(params->rgvarg[2])),
                                variantBytes(params->rgvarg[1]),
                                variantBytes(params->rgvarg[0]));

        return S_OK;
    }

private:
    LONG refCount;
    OmniRigClient *owner;
    IID eventsIid;
};

#endif

OmniRigClient::OmniRigClient(QObject *parent) :
    QObject(parent),
    omniRig(nullptr),
    rig(nullptr),
    connectionPoint(nullptr),
    sink(nullptr),
    cookie(0),
    openRigNumber(0),
    openVersion(0),
    comInitialized(false)
{
    FCT_IDENTIFICATION;
}

OmniRigClient::~OmniRigClient()
{
    FCT_IDENTIFICATION;

    close();
}

bool OmniRigClient::open(int version, int rigNumber)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << version << rigNumber;

    close();

#ifdef Q_OS_WIN
    if ( rigNumber < 1 || rigNumber > 4 )
        return false;

    // the GUI thread is normally a COM apartment already; this only counts it
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialized = SUCCEEDED(init);

    const GUID clsid = ( version == 2 ) ? GUID(OMNIRIG_V2_CLSID) : GUID(OMNIRIG_V1_CLSID);
    const GUID events = ( version == 2 ) ? GUID(OMNIRIG_V2_EVENTS) : GUID(OMNIRIG_V1_EVENTS);
    IDispatch *server = nullptr;

    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
                                  reinterpret_cast<void **>(&server));

    if ( FAILED(hr) || !server )
    {
        qCWarning(runtime) << "Cannot reach OmniRig, hr =" << QString::number(hr, 16);
        close();
        return false;
    }

    omniRig = server;

    const QString rigName = QStringLiteral("Rig%1").arg(rigNumber);
    VARIANT result;

    if ( !getProperty(server, reinterpret_cast<const wchar_t *>(rigName.utf16()), result)
         || result.vt != VT_DISPATCH || !result.pdispVal )
    {
        qCWarning(runtime) << "OmniRig has no" << rigName;
        VariantClear(&result);
        close();
        return false;
    }

    // the VARIANT's reference becomes ours
    rig = result.pdispVal;
    openRigNumber = rigNumber;
    openVersion = version;

    // without events only the custom commands are lost
    IConnectionPointContainer *container = nullptr;

    if ( SUCCEEDED(server->QueryInterface(IID_IConnectionPointContainer,
                                          reinterpret_cast<void **>(&container))) && container )
    {
        IConnectionPoint *point = nullptr;

        if ( SUCCEEDED(container->FindConnectionPoint(events, &point)) && point )
        {
            sink = new OmniRigReplySink(this, events);
            DWORD adviseCookie = 0;

            if ( SUCCEEDED(point->Advise(sink, &adviseCookie)) )
            {
                connectionPoint = point;
                cookie = adviseCookie;
            }
            else
            {
                qCWarning(runtime) << "OmniRig did not accept the event sink";
                sink->detach();
                sink->Release();
                sink = nullptr;
                point->Release();
            }
        }

        container->Release();
    }

    qCDebug(runtime) << "Reading" << rigName << "of OmniRig" << version
                     << "events:" << ( connectionPoint != nullptr );
    return true;
#else
    Q_UNUSED(version)
    Q_UNUSED(rigNumber)
    return false;
#endif
}

void OmniRigClient::close()
{
    FCT_IDENTIFICATION;

#ifdef Q_OS_WIN
    if ( connectionPoint )
    {
        IConnectionPoint *point = static_cast<IConnectionPoint *>(connectionPoint);
        point->Unadvise(cookie);
        point->Release();
    }

    if ( sink )
    {
        sink->detach();
        sink->Release();
    }

    if ( rig )
        static_cast<IDispatch *>(rig)->Release();

    if ( omniRig )
        static_cast<IDispatch *>(omniRig)->Release();

    if ( comInitialized )
        CoUninitialize();
#endif

    connectionPoint = nullptr;
    sink = nullptr;
    cookie = 0;
    rig = nullptr;
    omniRig = nullptr;
    openRigNumber = 0;
    openVersion = 0;
    comInitialized = false;
}

bool OmniRigClient::isOpen() const
{
    FCT_IDENTIFICATION;

    return rig != nullptr;
}

QString OmniRigClient::rigType()
{
    FCT_IDENTIFICATION;

#ifdef Q_OS_WIN
    VARIANT result;

    if ( !getProperty(static_cast<IDispatch *>(rig), L"RigType", result) )
        return QString();

    const QString type = ( result.vt == VT_BSTR )
                         ? QString::fromWCharArray(result.bstrVal, SysStringLen(result.bstrVal))
                         : QString();
    VariantClear(&result);
    return type;
#else
    return QString();
#endif
}

QString OmniRigClient::rigFile()
{
    FCT_IDENTIFICATION;

#ifdef Q_OS_WIN
    const QString type = rigType();

    if ( type.isEmpty() )
        return QString();

    QStringList dirs;

    /* OmniRig is a 32-bit COM server, so it is registered in the 32-bit view
       of the registry. The value may be quoted and carry arguments. */
    const QUuid clsid = ( openVersion == 2 ) ? OMNIRIG_V2_CLSID : OMNIRIG_V1_CLSID;
    const QSettings registry(QStringLiteral("HKEY_CLASSES_ROOT\\CLSID\\%1\\LocalServer32")
                             .arg(clsid.toString(QUuid::WithBraces)),
                             QSettings::Registry32Format);
    QString server = registry.value(QStringLiteral("Default")).toString().trimmed();
    server.remove(QChar('"'));

    const int exe = server.indexOf(QStringLiteral(".exe"), 0, Qt::CaseInsensitive);

    if ( exe >= 0 )
    {
        server.truncate(exe + 4);
        dirs << QFileInfo(server).absolutePath() + QStringLiteral("/Rigs");
    }

    const QString appData = qEnvironmentVariable("APPDATA");

    if ( !appData.isEmpty() )
        dirs << QDir::fromNativeSeparators(appData) + QStringLiteral("/Afreet/Rigs");

    for ( const QString &dir : static_cast<const QStringList &>(dirs) )
    {
        const QString candidate = dir + QChar('/') + type + QStringLiteral(".ini");

        if ( QFileInfo::exists(candidate) )
            return candidate;
    }
#endif

    return QString();
}

bool OmniRigClient::readVfos(qint64 &freqA, qint64 &freqB)
{
    FCT_IDENTIFICATION;

#ifdef Q_OS_WIN
    IDispatch *object = static_cast<IDispatch *>(rig);
    long status = 0;

    if ( !getLong(object, L"Status", status) || status != OMNIRIG_ONLINE )
        return false;

    long a = 0;
    long b = 0;

    if ( !getLong(object, L"FreqA", a) || !getLong(object, L"FreqB", b) )
        return false;

    freqA = a;
    freqB = b;
    return true;
#else
    Q_UNUSED(freqA)
    Q_UNUSED(freqB)
    return false;
#endif
}

bool OmniRigClient::sendCustomCommand(const QByteArray &command, int replyLength,
                                      const QByteArray &replyEnd)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << command << replyLength << replyEnd;

#ifdef Q_OS_WIN
    IDispatch *object = static_cast<IDispatch *>(rig);

    if ( !object || !connectionPoint || command.isEmpty() )
        return false;

    DISPID id = 0;
    LPOLESTR member = const_cast<LPOLESTR>(L"SendCustomCommand");

    if ( FAILED(object->GetIDsOfNames(IID_NULL, &member, 1, LOCALE_USER_DEFAULT, &id)) )
        return false;

    // arguments go last first: ReplyEnd, ReplyLength, Command
    VARIANT args[3];

    // no terminator is passed as an empty string, as OmniRig's own clients do
    if ( replyEnd.isEmpty() )
    {
        VariantInit(&args[0]);
        args[0].vt = VT_BSTR;
        args[0].bstrVal = SysAllocString(L"");
    }
    else
        args[0] = byteArrayVariant(replyEnd);

    VariantInit(&args[1]);
    args[1].vt = VT_I4;
    args[1].lVal = replyLength;
    args[2] = byteArrayVariant(command);

    DISPPARAMS params = { args, nullptr, 3, 0 };
    const HRESULT hr = object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                                      &params, nullptr, nullptr, nullptr);

    for ( VARIANT &arg : args )
        VariantClear(&arg);

    if ( FAILED(hr) )
        qCDebug(runtime) << "SendCustomCommand failed, hr =" << QString::number(hr, 16);

    return SUCCEEDED(hr);
#else
    Q_UNUSED(command)
    Q_UNUSED(replyLength)
    Q_UNUSED(replyEnd)
    return false;
#endif
}

void OmniRigClient::deliverReply(int rigNumber, const QByteArray &command, const QByteArray &reply)
{
    FCT_IDENTIFICATION;

    qCDebug(function_parameters) << rigNumber << command << reply;

    // the event is broadcast to all OmniRig clients
    if ( rigNumber != openRigNumber )
        return;

    emit customReply(command, reply);
}
