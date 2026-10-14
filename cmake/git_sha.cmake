# Run on every build (the blinx2_git_sha target in CMakeLists.txt): writes
# OUT with both repos' commits as BLINX2_GIT_SHA / XBOXRECOMP_GIT_SHA, so a
# recording's header names the code that made it even after a relink
# without a reconfigure. "+dirty" marks tracked changes not committed. The
# file is only rewritten when its text changes, so an unchanged tree does not
# recompile anything.
#   cmake -DOUT=<header> -DSRC=<blinx2-recomp dir> -DGIT=<git> -P git_sha.cmake

function(sha_of dir var)
    set(sha "unknown")
    if(GIT)
        execute_process(COMMAND ${GIT} rev-parse --short HEAD
                        WORKING_DIRECTORY ${dir}
                        OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE rc ERROR_QUIET)
        if(rc EQUAL 0 AND out)
            set(sha "${out}")
            execute_process(COMMAND ${GIT} diff --quiet HEAD --
                            WORKING_DIRECTORY ${dir}
                            RESULT_VARIABLE dirty ERROR_QUIET)
            if(NOT dirty EQUAL 0)
                string(APPEND sha "+dirty")
            endif()
        endif()
    endif()
    set(${var} "${sha}" PARENT_SCOPE)
endfunction()

sha_of("${SRC}" blinx2)
sha_of("${SRC}/../xboxrecomp" xboxrecomp)
set(text "/* Generated on every build by cmake/git_sha.cmake. */\n#define BLINX2_GIT_SHA \"${blinx2}\"\n#define XBOXRECOMP_GIT_SHA \"${xboxrecomp}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
