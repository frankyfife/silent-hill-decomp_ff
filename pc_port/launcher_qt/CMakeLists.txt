# Qt launcher for the Silent Hill PC port: game options, level select, Play.
# The C# launcher (pc_port/launcher) stays for the Mod Manager, asset tools and
# updates; this one opens it from "Mods & Tools".
#
# A separate project from the game: Qt here is the MSVC build, the game is
# MinGW. See README.md for the build and deploy commands.
cmake_minimum_required(VERSION 3.21)
project(SilentHillPC_LauncherQt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(SilentHillPC_LauncherQt WIN32
    src/main.cpp
    src/ConfigFile.cpp
    src/ConfigFile.h
    src/LauncherWindow.cpp
    src/LauncherWindow.h
    src/Options.h
    resources/launcher.qrc
)
target_link_libraries(SilentHillPC_LauncherQt PRIVATE Qt6::Widgets)
