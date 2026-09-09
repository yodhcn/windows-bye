cmake_minimum_required(VERSION 3.21)
project(windows_byte VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 第三方便携依赖根目录。
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party")
set(QT_DIR "${ROOT}/6.8.3/msvc2022_64")
set(OPENCV_DIR "${ROOT}/opencv/opencv/build")
set(INSPIRE_DIR "${ROOT}/InspireFace/build/install/InspireFace")

# ---- Qt ----
list(APPEND CMAKE_PREFIX_PATH "${QT_DIR}")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

qt_add_executable(windows-bye WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/PreviewWidget.cpp
    src/EngineWorker.cpp
    src/FaceEngine.cpp
)

target_include_directories(windows-bye PRIVATE
    src
    "${OPENCV_DIR}/include"
    "${INSPIRE_DIR}/include"
)

target_link_directories(windows-bye PRIVATE
    "${OPENCV_DIR}/x64/vc16/lib"
    "${INSPIRE_DIR}/lib"
)

target_link_libraries(windows-bye PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    opencv_world4100
    InspireFace
    user32
    Wtsapi32
)

# Windows 子系统，避免附带控制台窗口。
set_target_properties(windows-bye PROPERTIES
    WIN32_EXECUTABLE ON
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
)

# 让调试/发布均能找到运行依赖 DLL。
set_target_properties(windows-bye PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/dist/windows-bye"
)
