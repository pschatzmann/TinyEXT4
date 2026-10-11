# command line tool to inspect and change ext4 images
add_executable(ext4tool ext4tool.cpp)
target_link_libraries(ext4tool PRIVATE tinyext4)
