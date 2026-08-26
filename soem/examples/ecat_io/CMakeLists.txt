cmake_minimum_required(VERSION 3.22) # ask CMake for version >= 3.22

set(COMPONENTS main) # 缩小构建范围，从main组件开始构建(其依赖也要)

include($ENV{IDF_PATH}/tools/cmake/project.cmake) # add CMake build system designed for ESP-IDF

project(ecat_io) # define demo project name
