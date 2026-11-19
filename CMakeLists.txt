cmake_minimum_required(VERSION 3.16)

# PROJECT_VER is left unset. ESP-IDF then runs
# `git describe --always --tags --dirty` in this repo. A checkout of tag
# 0.1.1 gets version 0.1.1. The web update rejects an image that is not
# newer than the one running unless Force is checked.

# The ESP-IDF extension passes its selected chip in the environment. This
# board is an ESP32-C3, so the project target wins over that selection.
set(ENV{IDF_TARGET} "esp32c3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(bambutton)
