function(check_arm32_assembly)
  try_compile(HAVE_ARM32_ASM
    ${CMAKE_BINARY_DIR}/check_arm32_assembly
    SOURCES ${CMAKE_SOURCE_DIR}/cmake/source_arm32.s
  )
endfunction()
