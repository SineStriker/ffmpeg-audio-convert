file(GLOB _src *.h *.c *.cpp)
add_executable(${PROJECT_NAME} ${_src})

find_package(ffmpeg-fake REQUIRED NAMES FFmpeg)
ffmpeg_link_libraries(${PROJECT_NAME} PRIVATE avcodec avformat avutil swresample)