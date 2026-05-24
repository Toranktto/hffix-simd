if(HFFIX_BUILD_FIXSPEC_GEN)
    find_package(pugixml REQUIRED)
    add_executable(fixspec-gen-fields
        fixspec-gen-fields/fixspec-gen-fields.cpp)
    target_link_libraries(fixspec-gen-fields PRIVATE pugixml::pugixml)
    install(TARGETS fixspec-gen-fields RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(HFFIX_BUILD)
    add_executable(fixprint fixprint/fixprint.cpp)
    target_link_libraries(fixprint PRIVATE hffix)

    add_executable(fixgen fixgen/fixgen.cpp)
    target_link_libraries(fixgen PRIVATE hffix)

    install(TARGETS fixprint fixgen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
