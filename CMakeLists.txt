cmake_minimum_required(VERSION 2.8.3)
project(minerpp)

# if we are on a apple machine this is needed
if (1 AND APPLE)
	set(CMAKE_MACOSX_RPATH 1)
endif()

# find_library (LIBSTDCXX NAMES stdc++)


set(miner_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/libs/picohttpparser/picohttpparser.c
	${CMAKE_CURRENT_SOURCE_DIR}/libs/sphlib-3.0-c/shabal.c
)

include_directories("${PROJECT_SOURCE_DIR}/libs/nlohmann-json"
					"${PROJECT_SOURCE_DIR}/libs/picohttpparser/"
					"${PROJECT_SOURCE_DIR}/libs/sphlib-3.0-c/"
					"/usr/local/Cellar/gcc/7.3.0_1/include/c++/7.3.0")

# link_directories("${PROJECT_SOURCE_DIR}/libs/sphlib-3.0-c/")

add_executable(minerpp ${miner_SOURCES})
set_property(TARGET minerpp PROPERTY CXX_STANDARD 14)
set_property(TARGET minerpp PROPERTY CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "-O2")        ## Optimize

target_link_libraries(minerpp
					  stdc++fs
					  pthread
					  # ${LIBSTDCXX}
					  # stdc++
				  	  # libsph.a
				  	 )
