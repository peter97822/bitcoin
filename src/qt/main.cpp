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

#include <util/translation.h>
#include <util/url.h>

#include <QCoreApplication>

#include <functional>
#include <string>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_ANDROID)
Q_IMPORT_PLUGIN(QAndroidPlatformIntegrationPlugin)
#endif
#endif

/** Translate string to current locale using Qt. */
extern const std::function<std::string(const char*)> G_TRANSLATION_FUN = [](const char* psz) {
    return QCoreApplication::translate("bitcoin-core", psz).toStdString();
};
UrlDecodeFn* const URL_DECODE = urlDecode;

int main(int argc, char* argv[]) { return GuiMain(argc, argv); }
