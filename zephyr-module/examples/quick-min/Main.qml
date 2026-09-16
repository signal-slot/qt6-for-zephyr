// SPDX-License-Identifier: BSD-3-Clause
// Diagnostic (depth ordering), all opaque flat rectangles.  QML order = back
// to front, so the last one has the smallest depth (0).
//   (200,300)  red, alone
//   (512,80)   green, alone
//   (900,300)  blue with a smaller red rectangle declared after it (in front):
//              must read red (255,0,0)
//   (512,540)  yellow, declared last (front-most, depth 0), alone
import QtQuick

Window {
    width: 1024
    height: 600
    visible: true
    color: "black"

    Rectangle { x: 412; y: 30; width: 200; height: 100; color: "lime" }
    Rectangle { x: 100; y: 200; width: 200; height: 200; color: "red" }
    Rectangle { x: 800; y: 200; width: 200; height: 200; color: "blue" }
    Rectangle { x: 850; y: 250; width: 100; height: 100; color: "red" }
    Rectangle { x: 412; y: 490; width: 200; height: 100; color: "yellow" }
    // keeps frames coming (a static scene renders once): a small rectangle
    // moving in the bottom-right corner, front-most, away from the samples
    Rectangle {
        x: 990; y: 570; width: 12; height: 12; color: "white"
        SequentialAnimation on x {
            loops: Animation.Infinite
            NumberAnimation { to: 1010; duration: 500 }
            NumberAnimation { to: 990; duration: 500 }
        }
    }
}
