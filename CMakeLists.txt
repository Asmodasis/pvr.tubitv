cmake_minimum_required(VERSION 3.5)
project(pvr.tubitv)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

find_package(Kodi REQUIRED)
find_package(NlohmannJson REQUIRED)

include_directories(${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways
		    ${NLOHMANNJSON_INCLUDE_DIRS})

set(PVRAMAZON_SOURCES
                    src/addon.cpp
                    src/ClientInstance.cpp
                    src/TubitvData.cpp)

set(PVRAMAZON_HEADERS
                    
                    src/ClientInstance.h
                    src/TubitvData.h)

addon_version(pvr.tubitv IPTV)
add_definitions(-DIPTV_VERSION=${IPTV_VERSION})

build_addon(pvr.tubitv PVRAMAZON DEPLIBS)

include(CPack)
