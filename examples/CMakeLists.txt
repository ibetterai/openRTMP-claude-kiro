# OpenRTMP Examples
# CMakeLists.txt

# Simple server executable
add_executable(openrtmp_server
    simple_server.cpp
)

target_link_libraries(openrtmp_server
    PRIVATE
        openrtmp
)

# Platform-specific libraries
if(APPLE)
    target_link_libraries(openrtmp_server
        PRIVATE
            "-framework Foundation"
            "-framework CoreFoundation"
    )
endif()

if(WIN32)
    target_link_libraries(openrtmp_server
        PRIVATE
            ws2_32
            mswsock
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(openrtmp_server
        PRIVATE
            pthread
    )
endif()

# Install the executable
install(TARGETS openrtmp_server
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
