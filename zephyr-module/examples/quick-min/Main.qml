// SPDX-License-Identifier: BSD-3-Clause
// Diagnostic: one antialiased rectangle (smooth-colour material) at
// x 800..1000, y 200..400.  Read-back (900,300) is inside; every other
// point ((8,8), (8,591), (200,300), (512,80), (512,540)) must stay black.
import QtQuick

Window {
    width: 1024
    height: 600
    visible: true
    color: "black"

    Rectangle { x: 800; y: 200; width: 200; height: 200; color: "yellow"; antialiasing: true }
}
