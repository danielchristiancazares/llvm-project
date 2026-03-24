add_lld_unittests(LLDCOFFTests
  IncrementalPDBCacheTests.cpp
  IncrementalStateTests.cpp
)

target_link_libraries(LLDCOFFTests
  PRIVATE
  lldCOFF
)
