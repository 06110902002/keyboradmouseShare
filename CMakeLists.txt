cmake_minimum_required(VERSION 3.20)
project(keyboradmouseShare LANGUAGES CXX OBJCXX)

# C++20;核心代码用 C++/Objective-C++(.mm 访问 CoreGraphics / Carbon / AppKit)。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)

# 仅支持 macOS。
if(NOT APPLE)
    message(FATAL_ERROR "keyboradmouseShare 仅支持 macOS。")
endif()

add_executable(keyboradmouseShare
    main.mm
    src/Config.cpp
    src/Protocol.cpp
    src/NetServer.cpp        # 使用 CoreFoundation(CFSocket/CFRunLoop)
    src/KeyMap.mm            # 使用 Carbon(UCKeyTranslate)
    src/MacCapture.mm        # 使用 ApplicationServices(CGEventTap/光标)
    src/Controller.cpp       # 使用 CoreFoundation(CFRunLoopTimer)
)

target_include_directories(keyboradmouseShare PRIVATE src)

# 链接所需系统框架:
#   CoreFoundation      —— RunLoop / CFSocket / 定时器
#   ApplicationServices —— CGEventTap / CGEvent / 光标控制 / AXIsProcessTrusted
#   Carbon              —— UCKeyTranslate / TIS* / kVK_* 键码常量
#   AppKit              —— @autoreleasepool 及 Cocoa 运行环境
find_library(COREFOUNDATION_LIB CoreFoundation)
find_library(APPLICATIONSERVICES_LIB ApplicationServices)
find_library(CARBON_LIB Carbon)
find_library(APPKIT_LIB AppKit)

target_link_libraries(keyboradmouseShare PRIVATE
    ${COREFOUNDATION_LIB}
    ${APPLICATIONSERVICES_LIB}
    ${CARBON_LIB}
    ${APPKIT_LIB}
)
