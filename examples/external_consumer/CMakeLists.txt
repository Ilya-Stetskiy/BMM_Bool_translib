# examples/external_consumer/CMakeLists.txt — НЕ часть сборки bmm-translib
# (не подключается через add_subdirectory из корневого CMakeLists.txt) —
# это отдельный, самостоятельный CMake-проект, единственная цель которого —
# доказать, что `find_package(bmm-translib REQUIRED)` реально работает на
# УСТАНОВЛЕННОЙ (не в этом же дереве собранной) копии библиотеки.
#
# Разница с examples/quickstart_non_bdd.cpp принципиальна: тот собирается
# как обычный таргет в ТОМ ЖЕ build/ дереве, что и сама bmm-translib —
# линкует bmm_aig напрямую, find_package() ни разу не вызывается. Этот файл
# — единственное место в репозитории, где реально проверяется путь
# "cmake --install" -> "find_package из другого проекта", который
# `cmake/bmm-translibConfig.cmake.in`/README.md §4б описывают, но раньше
# никогда живьём не гоняли.
#
# Сборка (см. README.md/CI за полным примером с переменными):
#   cmake --install <build-bmm-translib> --prefix <install-prefix>
#   cmake -S examples/external_consumer -B build-consumer \
#         -DCMAKE_PREFIX_PATH="<install-prefix>;<BMM_DEPS_PREFIX>"
#   cmake --build build-consumer
#   ./build-consumer/bmm_external_consumer

cmake_minimum_required(VERSION 3.28)
project(bmm_external_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(bmm-translib REQUIRED)

add_executable(bmm_external_consumer main.cpp)
target_link_libraries(bmm_external_consumer PRIVATE bmm::bmm_aig bmm::bmm_core)
