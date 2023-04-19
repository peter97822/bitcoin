// Copyright (c) 2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

import org.bitcoincore.qt 1.0

import "../controls"

Item {
    id: root

    implicitWidth: 200
    implicitHeight: 200

    property alias header: mainText.text
    property alias headerSize: mainText.font.pixelSize
    property alias subText: subText.text
    property int headerSize: 32
    property bool connected: nodeModel.numOutboundPeers > 0
    property bool synced: nodeModel.verificationProgress > 0.999
    property string syncProgress: formatProgressPercentage(nodeModel.verificationProgress * 100)
    property bool paused: false
    property var syncState: formatRemainingSyncTime(nodeModel.remainingSyncTime)
    property string syncTime: syncState.text
    property bool estimating: syncState.estimating

    activeFocusOnTab: true

    BlockClockDial {
        id: dial
        anchors.fill: parent
        timeRatioList: chainModel.timeRatioList
        verificationProgress: nodeModel.verificationProgress
        paused: root.paused
        connected: root.connected
        synced: nodeModel.verificationProgress > 0.999
        backgroundColor: Theme.color.neutral2
        timeTickColor: Theme.color.neutral5
        confirmationColors: Theme.color.confirmationColors

        Behavior on backgroundColor {
            ColorAnimation { duration: 150 }
        }

        Behavior on timeTickColor {
            ColorAnimation { duration: 150 }
        }

        Behavior on confirmationColors {
            ColorAnimation { duration: 150 }
        }
    }

    Button {
        id: bitcoinIcon
        background: null
        icon.source: "image://images/bitcoin-circle"
        icon.color: Theme.color.neutral9
        icon.width: 40
        icon.height: 40
        anchors.bottom: mainText.top
        anchors.horizontalCenter: root.horizontalCenter

        Behavior on icon.color {
            ColorAnimation { duration: 150 }
        }
    }

    Label {
        id: mainText
        anchors.centerIn: parent
        font.family: "Inter"
        font.styleName: "Semi Bold"
        font.pixelSize: 32
        color: Theme.color.neutral9

        Behavior on color {
            ColorAnimation { duration: 150 }
        }
    }

    Label {
        id: subText
        anchors.top: mainText.bottom
        property bool estimating: root.estimating
        anchors.horizontalCenter: root.horizontalCenter
        font.family: "Inter"
        font.styleName: "Semi Bold"
        font.pixelSize: 18
        color: Theme.color.neutral4

        Component.onCompleted: {
            colorChanged.connect(function() {
                if (!subText.estimating) {
                    themeChange.restart();
                }
            });

            estimatingChanged.connect(function() {
                if (subText.estimating) {
                    estimatingTime.start();
                } else {
                    estimatingTime.stop();
                }
            });

            subText.estimatingChanged(subText.estimating);
        }

        ColorAnimation on color{
            id: themeChange
            target: subText
            duration: 150
        }

        SequentialAnimation {
            id: estimatingTime
            loops: Animation.Infinite
            ColorAnimation { target: subText; property: "color"; from: subText.color; to: Theme.color.neutral6; duration: 1000 }
            ColorAnimation { target: subText; property: "color"; from: Theme.color.neutral6; to: subText.color; duration: 1000 }
        }

    }

    PeersIndicator {
        anchors.top: subText.bottom
        anchors.topMargin: 20
        anchors.horizontalCenter: root.horizontalCenter
        numOutboundPeers: nodeModel.numOutboundPeers
        maxNumOutboundPeers: nodeModel.maxNumOutboundPeers
        paused: root.paused
    }

    MouseArea {
        anchors.fill: dial
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            root.paused = !root.paused
            nodeModel.pause = root.paused
        }
        FocusBorder {
            visible: root.activeFocus
        }
    }

    states: [
        State {
            name: "IBD"; when: !synced && !paused && connected
            PropertyChanges {
                target: root
                header: root.syncProgress
                subText: root.syncTime
            }
        },
        State {
            name: "BLOCKCLOCK"; when: synced && !paused && connected
            PropertyChanges {
                target: root
                header: Number(nodeModel.blockTipHeight).toLocaleString(Qt.locale(), 'f', 0)
                subText: "Blocktime"
                estimating: false
            }
        },

        State {
            name: "PAUSE"; when: paused
            PropertyChanges {
                target: root
                header: "Paused"
                headerSize: 24
                subText: "Tap to resume"
                estimating: false
            }
            PropertyChanges {
                target: bitcoinIcon
                anchors.bottomMargin: 5
            }
            PropertyChanges {
                target: subText
                anchors.topMargin: 4
            }
        },

        State {
            name: "CONNECTING"; when: !paused && !connected
            PropertyChanges {
                target: root
                header: "Connecting"
                headerSize: 24
                subText: "Please wait"
                estimating: false
            }
            PropertyChanges {
                target: bitcoinIcon
                anchors.bottomMargin: 5
            }
            PropertyChanges {
                target: subText
                anchors.topMargin: 4
            }
        }
    ]

    function formatProgressPercentage(progress) {
        if (progress >= 1) {
            return Math.round(progress) + "%"
        } else if (progress >= 0.1) {
            return progress.toFixed(1) + "%"
        } else if (progress >= 0.01) {
            return progress.toFixed(2) + "%"
        } else {
            return "0%"
        }
    }

    function formatRemainingSyncTime(milliseconds) {
        var minutes = Math.floor(milliseconds / 60000);
        var seconds = Math.floor((milliseconds % 60000) / 1000);
        var weeks = Math.floor(minutes / 10080);
        minutes %= 10080;
        var days = Math.floor(minutes / 1440);
        minutes %= 1440;
        var hours = Math.floor(minutes / 60);
        minutes %= 60;
        var result = "";
        var estimatingStatus = false;

        if (weeks > 0) {
            return {
                text: "~" + weeks + (weeks === 1 ? " week" : " weeks") + " left",
                estimating: false
            };
        }
        if (days > 0) {
            return {
                text: "~" + days + (days === 1 ? " day" : " days") + " left",
                estimating: false
            };
        }
        if (hours >= 5) {
            return {
                text: "~" + hours + (hours === 1 ? " hour" : " hours") + " left",
                estimating: false
            };
        }
        if (hours > 0) {
            return {
                text: "~" + hours + "h " + minutes + "m" + " left",
                estimating: false
            };
        }
        if (minutes >= 5) {
            return {
                text: "~" + minutes + (minutes === 1 ? " minute" : " minutes") + " left",
                estimating: false
            };
        }
        if (minutes > 0) {
            return {
                text: "~" + minutes + "m " + seconds + "s" + " left",
                estimating: false
            };
        }
        if (seconds > 0) {
            return {
                text: "~" + seconds + (seconds === 1 ? " second" : " seconds") + " left",
                estimating: false
            };
        } else {
            return {
                text: "Estimating",
                estimating: true
            };
        }
    }
}
