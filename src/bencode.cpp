#include "btclient/bencode.h"

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
    } // namespace btclient
}
