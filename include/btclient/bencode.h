#pragma once

#include <vector>
#include <variant>
#include <string>
#include <cstdint>

namespace btclient {
    struct Value {
        std::variant<std::int64_t, std::string, std::vector<Value>> data;
    };
} // namespace btclient
