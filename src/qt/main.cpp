// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef HAVE_CONFIG_H
#include <config/bitcoin-config.h>
#endif

#if USE_QML
#include <qml/bitcoin.h>
#else
#include <qt/bitcoin.h>
#endif // USE_QML

#include <logging.h>
#include <noui.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <util/url.h>

#include <functional>
#include <string>
#include <tuple>

#include <QCoreApplication>
#include <QString>

QT_BEGIN_NAMESPACE
class QMessageLogContext;
QT_END_NAMESPACE

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif

/** Translate string to current locale using Qt. */
extern const std::function<std::string(const char*)> G_TRANSLATION_FUN = [](const char* psz) {
    return QCoreApplication::translate("bitcoin-core", psz).toStdString();
};
UrlDecodeFn* const URL_DECODE = urlDecode;

namespace {
/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    Q_UNUSED(context);
    if (type == QtDebugMsg) {
        LogPrint(BCLog::QT, "GUI: %s\n", msg.toStdString());
    } else {
        LogPrintf("GUI: %s\n", msg.toStdString());
    }
}
} // namespace

int main(int argc, char* argv[])
{
#ifdef WIN32
    util::WinCmdLineArgs win_args;
    std::tie(argc, argv) = win_args.get();
#endif // WIN32

    SetupEnvironment();
    util::ThreadSetInternalName("main");

    // Subscribe to global signals from core.
    noui_connect();

    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);

#if USE_QML
    return QmlGuiMain(argc, argv);
#else
    return GuiMain(argc, argv);
#endif // USE_QML
}
