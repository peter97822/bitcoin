// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

AbstractButton {
    id: root
    property bool last: parent && root === parent.children[parent.children.length - 1]
    required property string header
    property alias actionItem: action_loader.sourceComponent
    property alias loadedItem: action_loader.item
    property string description
    property color stateColor
    state: "FILLED"

    states: [
        State {
            name: "FILLED"
            PropertyChanges {
                target: root
                enabled: true
                stateColor: Theme.color.neutral9
            }
        },
        State {
            name: "HOVER"
            PropertyChanges { target: root; stateColor: Theme.color.orangeLight1 }
        },
        State {
            name: "ACTIVE"
            PropertyChanges { target: root; stateColor: Theme.color.orange }
        },
        State {
            name: "DISABLED"
            PropertyChanges {
                target: root
                enabled: false
                stateColor: Theme.color.neutral4
            }
        }
    ]

    MouseArea {
        id: mouseArea
        anchors.fill: root
        hoverEnabled: true
        onEntered: {
            root.state = "HOVER"
        }
        onExited: {
            root.state = "FILLED"
        }
        onPressed: {
            root.state = "ACTIVE"
        }
        onReleased: {
            root.state = "HOVER"
            root.clicked()
        }
    }

    contentItem: ColumnLayout {
        spacing: 20
        width: parent.width
        RowLayout {
            Header {
                Layout.fillWidth: true
                center: false
                header: root.header
                headerSize: 18
                headerColor: root.stateColor
                description: root.description
                descriptionSize: 15
                descriptionMargin: 0
            }
            Loader {
                id: action_loader
                active: true
                visible: active
                sourceComponent: root.actionItem
            }
        }
        Loader {
            Layout.fillWidth: true
            Layout.columnSpan: 2
            active: !last
            visible: active
            sourceComponent: Rectangle {
                height: 1
                color: Theme.color.neutral5
            }
        }
    }
}
