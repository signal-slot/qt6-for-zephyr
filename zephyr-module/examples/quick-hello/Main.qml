// SPDX-License-Identifier: BSD-3-Clause
// First-light scene: a gradient background (two-colour rectangle), a rotating
// square, a bouncing circle, a frame counter and text.  Every element maps
// onto one of Qt Quick's built-in scene-graph materials (flat colour, smooth
// colour / vertex colour, distance-field text), so it exercises the shader
// table without ShaderEffect or images.
import QtQuick

Window {
    id: root
    width: 1024
    height: 600
    visible: true
    color: "#0b1018"

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#16233a" }
            GradientStop { position: 1.0; color: "#04060a" }
        }
    }

    Rectangle {
        id: square
        width: 160; height: 160
        x: 120; y: (root.height - height) / 2
        color: "#2fa8ff"
        radius: 18
        antialiasing: true
        RotationAnimation on rotation {
            from: 0; to: 360; duration: 4000
            loops: Animation.Infinite
        }
    }

    Rectangle {
        id: ball
        width: 96; height: 96; radius: 48
        color: "#ffb347"
        y: (root.height - height) / 2
        SequentialAnimation on x {
            loops: Animation.Infinite
            NumberAnimation { from: 340; to: root.width - 96 - 60; duration: 1500; easing.type: Easing.InOutQuad }
            NumberAnimation { from: root.width - 96 - 60; to: 340; duration: 1500; easing.type: Easing.InOutQuad }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 60
        text: "Qt 6 on Zephyr — OpenGL ES 2.0 via YakoGL"
        color: "white"
        font.pixelSize: 40
    }

    Text {
        id: counter
        property int frames: 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.height - 90
        color: "#9fb3c8"
        font.pixelSize: 28
        text: "frame " + frames + "   " + Math.round(fps.value) + " fps"
        Timer { interval: 1000; running: true; repeat: true; onTriggered: { fps.value = counter.frames - fps.last; fps.last = counter.frames } }
        QtObject { id: fps; property real value: 0; property int last: 0 }
    }

    // One increment per rendered frame (frameSwapped is emitted after each swap).
    Connections {
        target: root
        function onFrameSwapped() { counter.frames += 1 }
    }
}
