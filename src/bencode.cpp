#include "btclient/bencode.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace btclient {
    const Value* find(const Value& dictionary, std::string_view key) {
        const Dict* dict = std::get_if<Dict>(&dictionary.data);
        if (dict == nullptr) { return nullptr; }

        for (const auto& entry : *dict) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    namespace {
        // Decodes an integer of the form i<digits>e starting at `pos`, which must
        // point at the 'i'. On return, `pos` points just past the closing 'e'.
        Value decode_integer(std::string_view input, std::size_t& pos) {
            ++pos;  // skip 'i'

            const std::size_t end = input.find('e', pos);
            if (end == std::string_view::npos) {
                throw DecodeError("unterminated integer");
            }

            const std::string_view digits = input.substr(pos, end - pos);
            std::int64_t number{};
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), number);

            if (ec == std::errc::result_out_of_range) {
                throw DecodeError("integer out of range");
            }
            if (ec != std::errc{}) {
                throw DecodeError("invalid integer");
            }

            pos = end + 1;  // continue after 'e'
            return Value{number};
        }
    } // namespace

    Value decode(std::string_view input) {
        std::size_t pos = 0;
        return decode_integer(input, pos);
    }
} // namespace btclient
