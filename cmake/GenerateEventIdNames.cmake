# Parses the EventActorId enum in include/sf64event.h and writes a small C
# file that exposes EventId_GetName(int id) -> const char*. Used by the
# ObjectSpawnLog mod to print readable EVID names for OBJ_ACTOR_EVENT actors.
#
# Mirrors GenerateObjectIdNames.cmake; the parsing rules are the same. The
# EventActorId enum has non-contiguous explicit values (e.g. EVID_200 = 200,
# EVID_FFF = 4095) which the regex-based parser handles via the `= N` branch.

function(generate_event_id_names input_file output_file)
    if(NOT EXISTS "${input_file}")
        message(FATAL_ERROR "generate_event_id_names: missing ${input_file}")
    endif()

    file(READ "${input_file}" file_content)

    # Grab the enum body. `[^}]*` works because no nested braces appear in
    # the enum block.
    string(REGEX MATCH "typedef enum EventActorId \\{([^}]*)\\} EventActorId" _match "${file_content}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "generate_event_id_names: EventActorId enum not found in ${input_file}")
    endif()
    set(enum_body "${CMAKE_MATCH_1}")

    # CMake lists are semicolon-separated, so escape literal semicolons before
    # splitting on newlines.
    string(REPLACE ";" "\\;" enum_body "${enum_body}")
    string(REPLACE "\n" ";" enum_lines "${enum_body}")

    set(current_id 0)
    set(cases "")
    foreach(line IN LISTS enum_lines)
        string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" line "${line}")
        string(REGEX REPLACE "//.*" "" line "${line}")
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()

        if(line MATCHES "^([A-Z_][A-Z0-9_]*)[ \t]*=[ \t]*(-?[0-9]+)[, \t]*$")
            set(current_id "${CMAKE_MATCH_2}")
            set(name "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^([A-Z_][A-Z0-9_]*)[, \t]*$")
            set(name "${CMAKE_MATCH_1}")
        else()
            continue()
        endif()

        string(APPEND cases "        case ${current_id}: return \"${name}\";\n")
        math(EXPR current_id "${current_id} + 1")
    endforeach()

    if(cases STREQUAL "")
        message(FATAL_ERROR "generate_event_id_names: no enum entries parsed from ${input_file}")
    endif()

    set(generated_content
"// GENERATED FILE - DO NOT EDIT.
// Re-generated at CMake configure time from include/sf64event.h by
// cmake/GenerateEventIdNames.cmake. Edits will be overwritten.

#include \"ObjectSpawnLog.h\"

const char* EventId_GetName(int id) {
    switch (id) {
${cases}        default: return \"EVID_UNKNOWN\";
    }
}
")
    file(WRITE "${output_file}" "${generated_content}")
endfunction()
