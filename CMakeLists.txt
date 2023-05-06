cmake_minimum_required(VERSION 3.17)

set(PROJECT_NAME devenir)
project(${PROJECT_NAME} LANGUAGES CXX C VERSION 0.0.1 DESCRIPTION "devenir: Interactive Rigid/Non-Rigid ICP Tool")

set(CMAKE_VERBOSE_MAKEFILE TRUE)

# .lib
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# .dll and .exe
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin) 

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

if (WIN32)
# option for Visual Studio
# -EHsc (enable proper Exxeption Handling) needs to avoid C4530
# -Wall is too noisy so that set -W4. 
# https://docs.microsoft.com/en-us/cpp/build/reference/compiler-option-warning-level?view=vs-2017
# "However, for a new project, it may be best to use /W4 in all compilations;
# this will ensure the fewest possible hard-to-find code defects."
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /EHsc /MP /bigobj")

add_definitions(-DNOMINMAX)

else()
# g++/clang option for *nix
set(CMAKE_CXX_EXTENSIONS OFF) #Set this to ON if you want to use GNU++
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wall -O2")
endif()

get_directory_property(hasParent PARENT_DIRECTORY)
if(hasParent)
  message(STATUS "Has a parent scope.")
else()
  message(STATUS "Doesn't have a parent scope.")
endif()

function(set_as_cache ARG_NAME ARG_PATH ARG_TEXT)
    set(${ARG_NAME} ${ARG_PATH} CACHE PATH ${ARG_TEXT} FORCE)
endfunction(set_as_cache)

set_as_cache(UGU_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ugu" " ")
set_as_cache(IMGUI_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ugu/third_party/imgui" " ")
set_as_cache(GLAD_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ugu/third_party/glad" " ")

add_subdirectory(${UGU_INSTALL_DIR})

file(GLOB IMGUI_SOURCES ${IMGUI_INSTALL_DIR}/*.cpp)
file(GLOB IMGUI_HEADERS ${IMGUI_INSTALL_DIR}/*.h)
file(GLOB GLAD_HEADERS ${GLAD_INSTALL_DIR}/include/glad/*.h)
set(GLAD_SOURCES ${GLAD_INSTALL_DIR}/src/gl.c)

set(IMGUI_GL_SOURCES
    ${IMGUI_INSTALL_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_INSTALL_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_INSTALL_DIR}/backends/imgui_impl_opengl3.h
    ${IMGUI_INSTALL_DIR}/backends/imgui_impl_opengl3_loader.h
    ${IMGUI_INSTALL_DIR}/backends/imgui_impl_glfw.h)

add_executable(devenir app/main.cc ${IMGUI_SOURCES} ${IMGUI_HEADERS} ${IMGUI_GL_SOURCES} ${GLAD_HEADERS} ${GLAD_SOURCES})
target_include_directories(devenir PRIVATE ${Ugu_INCLUDE_DIRS} ${IMGUI_INSTALL_DIR} ${IMGUI_INSTALL_DIR}/backends)

if (WIN32)
  target_link_libraries(devenir ${Ugu_LIBS} opengl32)
else()
  target_link_libraries(devenir ${Ugu_LIBS} GL)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})


