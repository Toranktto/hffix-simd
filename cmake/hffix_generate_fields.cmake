# hffix_generate_fields(TARGET <tgt> SPEC_XML <xml> [<xml> ...])
# Regenerates hffix_fields.hpp + hffix_groups.hpp from QuickFIX-format
# XML and prepends the output dir to TARGET's includes so the new
# headers shadow the bundled ones.
#
# fixspec-gen-fields lookup order: in-tree target, imported
# hffix2::fixspec-gen-fields, binary at ../../bin/ relative to this file.

if(NOT TARGET fixspec-gen-fields AND NOT TARGET hffix2::fixspec-gen-fields)
    set(_hffix_fixspec_gen_path "")
    foreach(_c IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../../bin/fixspec-gen-fields"
            "${CMAKE_CURRENT_LIST_DIR}/../../bin/fixspec-gen-fields.exe")
        get_filename_component(_c_abs "${_c}" ABSOLUTE)
        if(EXISTS "${_c_abs}")
            set(_hffix_fixspec_gen_path "${_c_abs}")
            break()
        endif()
    endforeach()
    if(_hffix_fixspec_gen_path)
        add_executable(hffix2::fixspec-gen-fields IMPORTED)
        set_target_properties(hffix2::fixspec-gen-fields PROPERTIES
            IMPORTED_LOCATION "${_hffix_fixspec_gen_path}")
    endif()
    unset(_hffix_fixspec_gen_path)
    unset(_c_abs)
endif()

function(hffix_generate_fields)
    cmake_parse_arguments(HGF "" "TARGET" "SPEC_XML" ${ARGN})
    if(NOT HGF_TARGET OR NOT HGF_SPEC_XML)
        message(FATAL_ERROR
            "hffix_generate_fields requires TARGET and SPEC_XML "
            "(one or more QuickFIX-format specs, e.g. fixspec/FIX50SP2.xml fixspec/FIXT11.xml)")
    endif()
    if(NOT TARGET ${HGF_TARGET})
        message(FATAL_ERROR
            "hffix_generate_fields: TARGET '${HGF_TARGET}' does not exist; "
            "create it before calling this function")
    endif()
    if(TARGET fixspec-gen-fields)
        set(_gen_cmd "$<TARGET_FILE:fixspec-gen-fields>")
        set(_gen_dep fixspec-gen-fields)
    elseif(TARGET hffix2::fixspec-gen-fields)
        set(_gen_cmd "$<TARGET_FILE:hffix2::fixspec-gen-fields>")
        set(_gen_dep hffix2::fixspec-gen-fields)
    else()
        message(FATAL_ERROR
            "hffix_generate_fields needs the fixspec-gen-fields target. "
            "Either build hffix2 with HFFIX_BUILD_FIXSPEC_GEN=ON, or "
            "install the Conan package which ships the binary.")
    endif()
    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/hffix_fields_${HGF_TARGET}")
    set(_fields_out "${_out_dir}/hffix_fields.hpp")
    set(_groups_out "${_out_dir}/hffix_groups.hpp")
    set(_gen_target "${HGF_TARGET}__hffix_fields")

    add_custom_command(
        OUTPUT  ${_fields_out} ${_groups_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_out_dir}
        COMMAND ${_gen_cmd}
                ${HGF_SPEC_XML}
                -o ${_fields_out}
                -go ${_groups_out}
        DEPENDS ${_gen_dep} ${HGF_SPEC_XML}
        COMMENT "fixspec-gen-fields -> ${_fields_out} + ${_groups_out}"
        VERBATIM
    )

    add_custom_target(${_gen_target} DEPENDS ${_fields_out} ${_groups_out})
    add_dependencies(${HGF_TARGET} ${_gen_target})
    target_include_directories(${HGF_TARGET} BEFORE PRIVATE ${_out_dir})
endfunction()
