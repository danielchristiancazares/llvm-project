add_lld_unittests(LLDCOFFTests
  IncrementalStateTests.cpp
)

target_link_libraries(LLDCOFFTests
  PRIVATE
  lldCOFF
)
