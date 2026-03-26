add_lld_unittests(LLDCOFFTests
  DesignGuardrailTests.cpp
  IncrementalPDBCacheTests.cpp
  IncrementalStateTests.cpp
)

get_filename_component(LLD_TEST_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
file(TO_CMAKE_PATH "${LLD_TEST_REPO_ROOT}" LLD_TEST_REPO_ROOT)

target_compile_definitions(LLDCOFFTests
  PRIVATE
  LLD_TEST_REPO_ROOT="${LLD_TEST_REPO_ROOT}"
)

target_link_libraries(LLDCOFFTests
  PRIVATE
  lldCOFF
)
