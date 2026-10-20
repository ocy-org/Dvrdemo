cmake_minimum_required(VERSION 3.16)

project(Dvrqt VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Quick)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Quick Network Multimedia)

set(PROJECT_SOURCES
        main.cpp
        qml.qrc
        engine/RecordingEngine.cpp
        engine/Config.h
        engine/CameraController.h
        engine/RecordingEngine.h
        engine/SegmentMuxer.h
        engine/SegmentMuxerManager.h
        engine/StorageManager.h
        engine/VideoEncoder.h
        engine/DvrTcpServer.h
        engine/DvrProtoServer.h
        engine/RtspServer.h
        engine/GSensor.h
        engine/MediaLibrary.h
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(Dvrqt
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET Dvrqt APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(Dvrqt SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(Dvrqt
          ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(Dvrqt PRIVATE
    Qt6::Quick
    Qt6::Network
    Qt6::Multimedia
    mediandk     # AMediaCodec / AMediaFormat / AMediaMuxer
    camera2ndk   # ACameraManager / ACameraDevice / ACaptureRequest
    android      # ANativeWindow
    log          # __android_log_print
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.Dvrqt)
endif()
set_target_properties(Dvrqt PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    QT_ANDROID_MIN_SDK_VERSION 24        # AMediaMuxer 需要API 24
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
)

include(GNUInstallDirs)
install(TARGETS Dvrqt
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(QT_VERSION_MAJOR EQUAL 6)
    qt_import_qml_plugins(Dvrqt)
    qt_finalize_executable(Dvrqt)
endif()

