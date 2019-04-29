// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <init.h>
#include <qt/bitcoin.h>
#include <qt/test/optiontests.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <QSettings>
#include <QTest>

#include <univalue.h>

#include <fstream>

OptionTests::OptionTests(interfaces::Node& node) : m_node(node)
{
    gArgs.LockSettings([&](util::Settings& s) { m_previous_settings = s; });
}

void OptionTests::init()
{
    // reset args
    gArgs.LockSettings([&](util::Settings& s) { s = m_previous_settings; });
    gArgs.ClearPathCache();
}

void OptionTests::migrateSettings()
{
    // Set legacy QSettings and verify that they get cleared and migrated to
    // settings.json
    QSettings settings;
    settings.setValue("nDatabaseCache", 600);
    settings.setValue("nThreadsScriptVerif", 12);
    settings.setValue("fUseUPnP", false);
    settings.setValue("fListen", false);
    settings.setValue("fUseProxy", true);
    settings.setValue("addrProxy", "proxy:123");
    settings.setValue("fUseSeparateProxyTor", true);
    settings.setValue("addrSeparateProxyTor", "onion:234");

    settings.sync();

    OptionsModel options{m_node};
    bilingual_str error;
    QVERIFY(options.Init(error));
    QVERIFY(!settings.contains("nDatabaseCache"));
    QVERIFY(!settings.contains("nThreadsScriptVerif"));
    QVERIFY(!settings.contains("fUseUPnP"));
    QVERIFY(!settings.contains("fListen"));
    QVERIFY(!settings.contains("fUseProxy"));
    QVERIFY(!settings.contains("addrProxy"));
    QVERIFY(!settings.contains("fUseSeparateProxyTor"));
    QVERIFY(!settings.contains("addrSeparateProxyTor"));

    std::ifstream file(gArgs.GetDataDirNet() / "settings.json");
    QCOMPARE(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()).c_str(), "{\n"
        "    \"dbcache\": \"600\",\n"
        "    \"listen\": false,\n"
        "    \"onion\": \"onion:234\",\n"
        "    \"par\": \"12\",\n"
        "    \"proxy\": \"proxy:123\"\n"
        "}\n");
}

void OptionTests::integerGetArgBug()
{
    // Test regression https://github.com/bitcoin/bitcoin/issues/24457. Ensure
    // that setting integer prune value doesn't cause an exception to be thrown
    // in the OptionsModel constructor
    gArgs.LockSettings([&](util::Settings& settings) {
        settings.forced_settings.erase("prune");
        settings.rw_settings["prune"] = 3814;
    });
    gArgs.WriteSettingsFile();
    bilingual_str error;
    QVERIFY(OptionsModel{m_node}.Init(error));
    gArgs.LockSettings([&](util::Settings& settings) {
        settings.rw_settings.erase("prune");
    });
    gArgs.WriteSettingsFile();
}

void OptionTests::parametersInteraction()
{
    // Test that the bug https://github.com/bitcoin-core/gui/issues/567 does not resurface.
    // It was fixed via https://github.com/bitcoin-core/gui/pull/568.
    // With fListen=false in ~/.config/Bitcoin/Bitcoin-Qt.conf and all else left as default,
    // bitcoin-qt should set both -listen and -listenonion to false and start successfully.
    gArgs.LockSettings([&](util::Settings& s) {
        s.forced_settings.erase("listen");
        s.forced_settings.erase("listenonion");
    });
    QVERIFY(!gArgs.IsArgSet("-listen"));
    QVERIFY(!gArgs.IsArgSet("-listenonion"));

    QSettings settings;
    settings.setValue("fListen", false);

    bilingual_str error;
    QVERIFY(OptionsModel{m_node}.Init(error));

    const bool expected{false};

    QVERIFY(gArgs.IsArgSet("-listen"));
    QCOMPARE(gArgs.GetBoolArg("-listen", !expected), expected);

    QVERIFY(gArgs.IsArgSet("-listenonion"));
    QCOMPARE(gArgs.GetBoolArg("-listenonion", !expected), expected);

    QVERIFY(AppInitParameterInteraction(gArgs));

    // cleanup
    settings.remove("fListen");
    QVERIFY(!settings.contains("fListen"));
    gArgs.ClearPathCache();
}
