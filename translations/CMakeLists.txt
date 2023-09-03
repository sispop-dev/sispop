# Copyright (c) 2020, The Loki Project
#
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are
# permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this list of
#    conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice, this list
#    of conditions and the following disclaimer in the documentation and/or other
#    materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors may be
#    used to endorse or promote products derived from this software without specific
#    prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
# THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
# THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

find_package(Qt5 QUIET COMPONENTS Core LinguistTools)
if(NOT Qt5_FOUND OR NOT Qt5LinguistTools_FOUND)
  set(qm_files "")
  message(WARNING "Qt5 LingustTools not found, translation files not built")
else()
  file(GLOB ts_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" *.ts)
  qt5_add_translation(qm_files ${ts_files})
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/translation_files.cpp
    COMMAND ${CMAKE_COMMAND}
        -D "qm_files=${qm_files}"
        -D "base_dir=${CMAKE_CURRENT_BINARY_DIR}"
        -D "in_file=${CMAKE_CURRENT_SOURCE_DIR}/translation_files.cpp.in"
        -D "out_file=${CMAKE_CURRENT_BINARY_DIR}/translation_files.cpp"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/generate_translation_data.cmake
    DEPENDS ${qm_files} ${CMAKE_CURRENT_SOURCE_DIR}/generate_translation_data.cmake
    VERBATIM
)
add_custom_target(generate_translation_data DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/translation_files.cpp)
