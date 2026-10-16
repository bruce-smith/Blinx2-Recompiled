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
# The generated C's hash is of its text with CRLF read as LF: the pipeline
# writes CRLF on Windows and LF on Linux, and the same code must match across
# them. Hashing ~130 MB of text takes seconds, so the result is cached
# against the files' names, sizes and times (${OUT}.gen) and only recomputed
# after a regeneration.
set(gen "none")
if(GEN)
    file(GLOB gen_files "${GEN}/*.c" "${GEN}/*.h")
    list(SORT gen_files)
    set(sig "")
    foreach(f IN LISTS gen_files)
        file(SIZE "${f}" fs)
        file(TIMESTAMP "${f}" ft "%Y%m%d%H%M%S" UTC)
        get_filename_component(fn "${f}" NAME)
        string(APPEND sig "${fn}:${fs}:${ft},")
    endforeach()
    set(cached "")
    if(EXISTS "${OUT}.gen")
        file(READ "${OUT}.gen" cached)
    endif()
    string(FIND "${cached}" "${sig}=" at)
    if(sig AND at EQUAL 0)
        string(LENGTH "${sig}=" n)
        string(SUBSTRING "${cached}" ${n} -1 gen)
    elseif(sig)
        set(all "")
        foreach(f IN LISTS gen_files)
            file(READ "${f}" body)
            string(REPLACE "\r\n" "\n" body "${body}")
            string(SHA1 fh "${body}")
            get_filename_component(fn "${f}" NAME)
            string(APPEND all "${fn}:${fh},")
        endforeach()
        string(SHA1 gen "${all}")
        string(SUBSTRING "${gen}" 0 12 gen)
        file(WRITE "${OUT}.gen" "${sig}=${gen}")
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
