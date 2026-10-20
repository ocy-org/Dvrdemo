cmake_minimum_required(VERSION 3.16)

project(DvrConsole LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QtCreator supports the following variables for Android, which are identical to qmake Android variables.
# Check http://doc.qt.io/qt-5/deployment-android.html for more information.
# They need to be set before the find_package(Qt5 ...) call.

#if(ANDROID)
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
#    if (ANDROID_ABI STREQUAL "armeabi-v7a")
#        set(ANDROID_EXTRA_LIBS
#            ${CMAKE_CURRENT_SOURCE_DIR}/path/to/libcrypto.so
#            ${CMAKE_CURRENT_SOURCE_DIR}/path/to/libssl.so)
#    endif()
#endif()

find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets Network REQUIRED)

# GL窗口模块按Qt版本分家：Qt6拆出OpenGLWidgets，Qt5在OpenGL里
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS OpenGLWidgets REQUIRED)
    set(DVR_GL_LIB Qt${QT_VERSION_MAJOR}::OpenGLWidgets)
else()
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS OpenGL REQUIRED)
    set(DVR_GL_LIB Qt${QT_VERSION_MAJOR}::OpenGL)
endif()

# ============ ★ 视频库：由 opencv.pri / v8ffmpeg.pri 翻译而来 ============
if(WIN32)
    # ---- OpenCV（源：D:/software/opencv/opencv.pri）----
    set(OPENCV_ROOT   "D:/software/opencv/opencvMinGW/OpenCV-MinGW-Build-OpenCV-4.5.2-x64")
    set(OPENCV_INC    "${OPENCV_ROOT}/include")                 # ↔ INCLUDEPATH +=$$OPENCV_ROOT/include
    set(OPENCV_LIBDIR "${OPENCV_ROOT}/x64/mingw/bin")           # ↔ -L$$OPENCV_LIB_PATH
    set(OPENCV_LIBS   opencv_core452 opencv_highgui452 opencv_imgproc452
                      opencv_calib3d452 opencv_imgcodecs452 opencv_videoio452)  # ↔ -l六个

    # ---- FFmpeg（源：D:/software/ffmpeg-8.0/v8ffmpeg.pri）----
    set(FFMPEG_ROOT    "D:/software/ffmpeg-8.0")
    set(FFMPEG_INC     "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBDIRS "${FFMPEG_ROOT}/bin" "${FFMPEG_ROOT}/lib")  # ↔ -L两个目录都保留
    # 顺序有讲究：上层库在前、avutil垫底（链接器从左往右解析）。
    # .pri里avutil写在最前是个潜伏bug，MinGW直接链DLL时可能踩中，这里修正。
    set(FFMPEG_LIBS    avformat avdevice avcodec avfilter swscale swresample avutil)

    add_library(vOpenCV INTERFACE)                     # INTERFACE库=路径+库的"打包器"，不产码
    target_include_directories(vOpenCV INTERFACE ${OPENCV_INC})
    target_link_directories(vOpenCV    INTERFACE ${OPENCV_LIBDIR})
    target_link_libraries(vOpenCV      INTERFACE ${OPENCV_LIBS})

    add_library(vFFmpeg INTERFACE)
    target_include_directories(vFFmpeg INTERFACE ${FFMPEG_INC})
    target_link_directories(vFFmpeg    INTERFACE ${FFMPEG_LIBDIRS})
    target_link_libraries(vFFmpeg      INTERFACE ${FFMPEG_LIBS})

    set(DVR_VIDEO_LIBS vOpenCV vFFmpeg)
    message(STATUS "[DVR] OpenCV : ${OPENCV_LIBDIR} (${OPENCV_LIBS})")
    message(STATUS "[DVR] FFmpeg : ${FFMPEG_LIBDIRS} (${FFMPEG_LIBS})")
else()
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil)
    set(DVR_VIDEO_LIBS ${OpenCV_LIBS} PkgConfig::FFMPEG)
endif()

if(ANDROID)
  add_library(DvrConsole SHARED
    main.cpp
    mainwindow.cpp
    mainwindow.h
  )
else()
  add_executable(DvrConsole
    main.cpp
    mainwindow.cpp
    mainwindow.h
    client/VideoWorker.h
    client/VideoWorker.cpp
    client/yuvglwidget.h
    client/yuvglwidget.cpp
    client/DeviceClient.h
    client/DeviceDiscoverer.h
    client/ScenarioReplayer.h
    client/ThumbTool.h
    client/VideoFrame.h
    client/FrameHub.h
  )
endif()

target_link_libraries(DvrConsole PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    ${DVR_GL_LIB}
    ${DVR_VIDEO_LIBS})

# ============ ★ 运行时DLL拷贝（等价两个.pri的win32拷贝段） ============
# .pri逐个列文件名拷到install子目录；这里GLOB全部dll拷到exe旁——运行时即刻可用
if(WIN32)
    file(GLOB DVR_RUNTIME_DLLS
        "${OPENCV_ROOT}/x64/mingw/bin/*.dll"     # 含libstdc++-6.dll等MinGW运行时，一并带走
        "${FFMPEG_ROOT}/bin/*.dll")
    foreach(dll ${DVR_RUNTIME_DLLS})
        add_custom_command(TARGET DvrConsole POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}"
                    $<TARGET_FILE_DIR:DvrConsole>)
    endforeach()
endif()
