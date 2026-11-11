cmake_minimum_required(VERSION 3.20)
project(PU-SignUpTiny VERSION 2.1.2 DESCRIPTION "OD - MyPUSignUp - Tiny Command-Line Sign-Up Tool" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(pu-signup-tiny
    main.cpp
    src/http.cpp
)

target_include_directories(pu-signup-tiny PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    target_link_libraries(pu-signup-tiny PRIVATE winhttp bcrypt crypt32)
else()
    find_package(CURL REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    find_package(Threads REQUIRED)
    target_link_libraries(pu-signup-tiny PRIVATE CURL::libcurl OpenSSL::Crypto Threads::Threads)
endif()

if(MSVC)
    target_compile_options(pu-signup-tiny PRIVATE /W4 /utf-8)
else()
    target_compile_options(pu-signup-tiny PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    target_link_options(pu-signup-tiny PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        $<$<CONFIG:Release>:-s>
    )
endif()

include(GNUInstallDirs)
install(TARGETS pu-signup-tiny RUNTIME DESTINATION .)
install(FILES
    config.example.json
    mycourse.example.json
    README.md
    LICENSE
    myqrcode.html
    DESTINATION .
)

set(CPACK_PACKAGE_NAME "pu-signup-tiny")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OD - MyPUSignUp - Tiny Command-Line Sign-Up Tool")
if(WIN32)
    set(CPACK_SYSTEM_NAME "windows-x64")
else()
    set(CPACK_SYSTEM_NAME "linux-x64")
endif()
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_SYSTEM_NAME}")
set(CPACK_GENERATOR "ZIP")
include(CPack)
