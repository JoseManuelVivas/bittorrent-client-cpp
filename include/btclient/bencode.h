#pragma once

#include <variant>
#include <string>
#include <cstdint>

namespace btclient {
    struct Value {
        std::variant<std::int64_t, std::string> data;
    };
} // namespace btclient