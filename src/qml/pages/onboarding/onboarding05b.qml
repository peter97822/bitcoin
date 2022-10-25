// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../../controls"
import "../../components"

Page {
    background: null
    Layout.fillWidth: true
    clip: true
    header: NavigationBar {
        rightDetail: NavButton {
            text: "Done"
            onClicked: {
                storages.decrementCurrentIndex()
                swipeView.inSubPage = false
            }
        }
    }
    ColumnLayout {
        width: Math.min(parent.width, 450)
        spacing: 0
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        Header {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            bold: true
            header: "Storage settings"
        }
        StorageSettings {
            Layout.fillWidth: true
            Layout.topMargin: 30
            Layout.leftMargin: 20
            Layout.rightMargin: 20
        }
    }
}
