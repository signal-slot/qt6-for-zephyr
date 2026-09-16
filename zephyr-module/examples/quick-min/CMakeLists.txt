# quick-min: one flat rectangle on a black window, the smallest possible
# scene-graph draw for isolating rendering problems on the GPU.
# An ordinary Qt app: it also builds and runs on a desktop Qt 6.
cmake_minimum_required(VERSION 3.16)
project(quick_min LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(quick_min main.cpp)
qt_add_qml_module(quick_min
    URI QuickMin
    VERSION 1.0
    QML_FILES Main.qml
)
target_link_libraries(quick_min PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
