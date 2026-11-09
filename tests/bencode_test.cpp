#include <gtest/gtest.h>
#include <string>
#include "btclient/bencode.h"

TEST(Find, ReturnsValueForExistingKey) {
    btclient::Value dictionary{btclient::Dict{{"length", btclient::Value{5}}}};

    const btclient::Value* item = btclient::find(dictionary, "length");

    ASSERT_NE(item, nullptr);
    EXPECT_EQ(*item, btclient::Value{5});
}

TEST(Find, ReturnsNullForMissingKey) {
    btclient::Value dictionary{btclient::Dict{{"length", btclient::Value{5}}}};

    const btclient::Value* item = btclient::find(dictionary, "comment");

    EXPECT_EQ(item, nullptr);
}

TEST(Find, ReturnsNullForEmptyDict) {
    btclient::Value dictionary{btclient::Dict{}};

    const btclient::Value* item = btclient::find(dictionary, "length");

    EXPECT_EQ(item, nullptr);
}

TEST(Find, ReturnsNullForNonDict) {
    {
        SCOPED_TRACE("string");
        btclient::Value value{std::string("dummy")};
        EXPECT_EQ(btclient::find(value, "length"), nullptr);
    }

    {
        SCOPED_TRACE("integer");
        btclient::Value value{5};
        EXPECT_EQ(btclient::find(value, "length"), nullptr);
    }

    {
        SCOPED_TRACE("list");
        btclient::Value value{btclient::List{}};
        EXPECT_EQ(btclient::find(value, "length"), nullptr);
    }
}

TEST(Find, RequiresExactKeyMatch) {
    btclient::Value dictionary{btclient::Dict{{"length", btclient::Value{5}}}};
    {
        SCOPED_TRACE("shorter");
        EXPECT_EQ(btclient::find(dictionary, "len"), nullptr);
    }

    {
        SCOPED_TRACE("longer");
        EXPECT_EQ(btclient::find(dictionary, "lengthy"), nullptr);
    }

    {
        SCOPED_TRACE("uppercase");
        EXPECT_EQ(btclient::find(dictionary, "Length"), nullptr);
    }

    {
        SCOPED_TRACE("empty string");
        EXPECT_EQ(btclient::find(dictionary, ""), nullptr);
    }
}

TEST(Find, FindsCorrectKeyAmongSeveral) {
    btclient::Value dictionary{btclient::Dict{
        {"length", btclient::Value{5}},
        {"comment", btclient::Value{std::string("bittorrent_client")}}
    }};

    const btclient::Value* item = btclient::find(dictionary, "comment");

    ASSERT_NE(item, nullptr);
    EXPECT_EQ(*item, btclient::Value{std::string("bittorrent_client")});
}

TEST(Find, ReturnsPointerIntoDictionary) {
    btclient::Value dictionary{btclient::Dict{{"length", btclient::Value{5}}}};

    const btclient::Value* item = btclient::find(dictionary, "length");

    // The result must point at the element stored inside the dictionary,
    // not at a temporary copy that would dangle after find() returns.
    const auto& entries = std::get<btclient::Dict>(dictionary.data);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item, &entries[0].second);
}
