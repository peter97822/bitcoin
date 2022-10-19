// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

import QtQuick 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.11
import org.bitcoincore.qt 1.0
import "../../controls"
import "../../components"

Page {
    background: null
    Layout.fillWidth: true
    clip: true
    header: NavigationBar {
        leftDetail: NavButton {
            iconSource: "image://images/caret-left"
            text: "Back"
            onClicked: swipeView.currentIndex -= 1
        }
    }
    ColumnLayout {
        id: selections
        width: 600
        spacing: 0
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        Header {
            Layout.fillWidth: true
            bold: true
            header: qsTr("Storage")
            description: qsTr("Data retrieved from the Bitcoin network is stored\non your device.\n\nYou have 500GB of storage available.")
        }
        StorageOptions {
            Layout.topMargin: 30
            Layout.alignment: Qt.AlignCenter
        }
        TextButton {
            Layout.alignment: Qt.AlignCenter
            Layout.topMargin: 30
            text: "Detailed settings"
            textSize: 18
            textColor: "#F7931A"
            onClicked: {
              storages.incrementCurrentIndex()
              swipeView.inSubPage = true
            }
        }
    }
    ContinueButton {
        id: continueButton
        anchors.topMargin: 40
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        anchors.bottomMargin: 60
        anchors.horizontalCenter: parent.horizontalCenter
        text: "Next"
        onClicked: swipeView.incrementCurrentIndex()
    }

    state: AppMode.state

    states: [
        State {
            name: "MOBILE"
            AnchorChanges {
                target: continueButton
                anchors.top: undefined
                anchors.bottom: continueButton.parent.bottom
                anchors.left: continueButton.parent.left
                anchors.right: continueButton.parent.right
            }
        },
        State {
            name: "DESKTOP"
            AnchorChanges {
                target: continueButton
                anchors.top: selections.bottom
                anchors.bottom: undefined
                anchors.left: undefined
                anchors.right: undefined
            }
        }
    ]
}
