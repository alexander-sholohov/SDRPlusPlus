cmake_minimum_required(VERSION 3.13)
project(omnirig_client)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(omnirig_client PRIVATE "../../decoder_modules/radio/src")
