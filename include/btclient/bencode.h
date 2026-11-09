#pragma once

#include <vector>
#include <utility>
#include <variant>
#include <string>
#include <cstdint>
#include <string_view>

namespace btclient {
    // Lists and dictionaries contain Values, so the type must be declared
    // before the aliases that refer to it.
    struct Value;
    using List = std::vector<Value>;
    // A vector of pairs instead of std::map: std::vector is guaranteed to work
    // with incomplete types (std::map is not), and it preserves the key order
    // of the original bencoded data.
    using Dict = std::vector<std::pair<std::string, Value>>;

    // A decoded bencode value: integer, byte string, list or dictionary.
    struct Value {
        std::variant<std::int64_t, std::string, List, Dict> data;
         bool operator==(const Value&) const = default;
    };

    // Returns the value associated with `key`, or nullptr if `dictionary` is not
    // a Dict or does not contain the key. The pointer refers to an element inside
    // `dictionary` and is only valid while it is alive and unmodified.
    [[nodiscard]] const Value* find(const Value& dictionary, std::string_view key);

} // namespace btclient
