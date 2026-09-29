cmake_minimum_required(VERSION 3.25)

project(TemDownstreamConsumer VERSION 1.0.0 LANGUAGES CXX)

# Deliberately independent: this project does not add the repository as a
# subdirectory and does not know where its build tree is. It only consumes the
# installed package through find_package.
find_package(ThermalEmergencyManager 1.0 REQUIRED)

add_executable(tem_consumer main.cpp)
target_link_libraries(tem_consumer PRIVATE Summon::tem)
target_compile_features(tem_consumer PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(tem_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(tem_consumer PRIVATE -Wall -Wextra -Werror)
endif()
