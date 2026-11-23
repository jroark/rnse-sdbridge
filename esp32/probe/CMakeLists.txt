# The slot's three wires on an ESP32-S3, listened to and nothing driven
# (docs/sdbridge.md, "The ESP32-S3"): built by esp32/probe.sh.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3probe)
