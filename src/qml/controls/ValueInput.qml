// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

import QtQuick 2.15
import QtQuick.Controls 2.15

TextInput {
    id: root
    required property string parentState
    property string description: ""
    property int descriptionSize: 18
    property color textColor: Theme.color.neutral9
    enabled: true
    state: root.parentState

    states: [
        State {
            name: "ACTIVE"
            PropertyChanges { target: root; textColor: Theme.color.orange }
        },
        State {
            name: "HOVER"
            PropertyChanges { target: root; textColor: Theme.color.orangeLight1 }
        },
        State {
            name: "DISABLED"
            PropertyChanges {
                target: root
                enabled: false
                textColor: Theme.color.neutral4
            }
        }
    ]

    font.family: "Inter"
    font.styleName: "Regular"
    font.pixelSize: root.descriptionSize
    color: root.textColor
    text: root.description
    horizontalAlignment: Text.AlignRight
    wrapMode: Text.WordWrap

    Behavior on color {
        ColorAnimation { duration: 150 }
    }
}
