// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../../controls"
import "../../components"

InformationPage {
    bannerActive: false
    bold: true
    headerText: qsTr("Developer options")
    headerMargin: 0
    detailActive: true
    detailItem: DeveloperOptions {}
}
