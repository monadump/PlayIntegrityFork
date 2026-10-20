#pragma once

#include <map>
#include <string>
#include <string_view>

namespace pif {
struct Config {
    std::map<std::string, std::string> propMap;
    bool debug = false;
    std::string deviceInitialSdkInt = "32";
    std::string securityPatch;
    std::string buildId;
};
}
