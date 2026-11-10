#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "cui::cui" for configuration "Release"
set_property(TARGET cui::cui APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(cui::cui PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libcui.a"
  )

list(APPEND _cmake_import_check_targets cui::cui )
list(APPEND _cmake_import_check_files_for_cui::cui "${_IMPORT_PREFIX}/lib/libcui.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
