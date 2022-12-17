// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../controls"

ColumnLayout {
    ButtonGroup {
        id: group
    }
    spacing: 15
    OptionButton {
        Layout.fillWidth: true
        ButtonGroup.group: group
        text: qsTr("Default")
        description: qsTr("Your application directory.")
        recommended: true
        checked: true
    }
    OptionButton {
        Layout.fillWidth: true
        ButtonGroup.group: group
        text: qsTr("Custom")
        description: qsTr("Choose the directory and storage device.")
    }
}
