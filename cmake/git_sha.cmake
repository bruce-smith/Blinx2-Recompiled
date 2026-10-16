# Run on every build (the blinx2_git_sha target in CMakeLists.txt): writes
# OUT with both repos' commits as BLINX2_GIT_SHA / XBOXRECOMP_GIT_SHA, so a
# recording's header names the code that made it even after a relink
# without a reconfigure. "+dirty" marks tracked changes not committed. The
# file is only rewritten when its text changes, so an unchanged tree does not
# recompile anything.
# RECOMP_GEN_HASH: the generated C (gitignored, so no commit names it) as a
# hash of every file's hash; a network game's join check compares it.
#   cmake -DOUT=<header> -DSRC=<blinx2-recomp dir> -DGIT=<git> [-DGEN=<gen dir>]
#         -P git_sha.cmake

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
set(gen "none")
if(GEN)
    file(GLOB gen_files "${GEN}/*.c" "${GEN}/*.h")
    list(SORT gen_files)
    set(all "")
    foreach(f IN LISTS gen_files)
        file(SHA1 "${f}" fh)
        get_filename_component(fn "${f}" NAME)
        string(APPEND all "${fn}:${fh};")
    endforeach()
    if(all)
        string(SHA1 gen "${all}")
        string(SUBSTRING "${gen}" 0 12 gen)
    endif()
endif()
set(text "/* Generated on every build by cmake/git_sha.cmake. */\n#define BLINX2_GIT_SHA \"${blinx2}\"\n#define XBOXRECOMP_GIT_SHA \"${xboxrecomp}\"\n#define RECOMP_GEN_HASH \"${gen}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
