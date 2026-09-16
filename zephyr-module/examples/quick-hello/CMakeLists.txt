# quick-hello: the smallest Qt Quick application, used as the first-light
# test of GPU rendering on Zephyr (west build ... -DQT_APP_DIR=<this dir>).
# An ordinary Qt app: it also builds and runs on a desktop Qt 6.
cmake_minimum_required(VERSION 3.16)
project(quick_hello LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(quick_hello main.cpp)
qt_add_qml_module(quick_hello
    URI QuickHello
    VERSION 1.0
    QML_FILES Main.qml
)
target_link_libraries(quick_hello PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
