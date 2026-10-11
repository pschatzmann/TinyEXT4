# Desktop ctest suite. The file system tests create images with mke2fs and
# verify the result with e2fsck and debugfs (e2fsprogs): without them the
# tests are reported as skipped.

function(tinyext4_add_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE tinyext4)
endfunction()

tinyext4_add_test(test_util)
add_test(NAME util COMMAND test_util)

tinyext4_add_test(test_ramio)
add_test(NAME ramio COMMAND test_ramio)

tinyext4_add_test(test_partition)
add_test(NAME partition COMMAND test_partition)

tinyext4_add_test(test_ext4)
foreach(config ext4_4k ext4_1k ext4_2k_groups ext4_nocsum ext4_no64bit
               ext4_nojournal ext4_inode128 ext4_mbr ext3 ext2)
  add_test(NAME ${config} COMMAND test_ext4 ${config})
endforeach()

get_property(_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_tests} PROPERTIES
  TIMEOUT 300
  SKIP_RETURN_CODE 77
  ENVIRONMENT "ASAN_OPTIONS=verify_asan_link_order=0")
