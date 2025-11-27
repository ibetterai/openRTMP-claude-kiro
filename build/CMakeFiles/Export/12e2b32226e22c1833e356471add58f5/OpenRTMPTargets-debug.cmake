#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenRTMP::openrtmp" for configuration "Debug"
set_property(TARGET OpenRTMP::openrtmp APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(OpenRTMP::openrtmp PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libopenrtmp.a"
  )

list(APPEND _cmake_import_check_targets OpenRTMP::openrtmp )
list(APPEND _cmake_import_check_files_for_OpenRTMP::openrtmp "${_IMPORT_PREFIX}/lib/libopenrtmp.a" )

# Import target "OpenRTMP::openrtmp_core" for configuration "Debug"
set_property(TARGET OpenRTMP::openrtmp_core APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(OpenRTMP::openrtmp_core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libopenrtmp_core.a"
  )

list(APPEND _cmake_import_check_targets OpenRTMP::openrtmp_core )
list(APPEND _cmake_import_check_files_for_OpenRTMP::openrtmp_core "${_IMPORT_PREFIX}/lib/libopenrtmp_core.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
