# services/portal module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(services/portal
  SOURCES
    portal_content.cpp
  DEPENDS
    distribution
  LIBRARIES
    nlohmann_json::nlohmann_json
)
