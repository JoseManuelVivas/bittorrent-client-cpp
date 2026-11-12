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

TEST(ValueEquality, EqualIntegersAreEqual) {
    btclient::Value a{5};
    btclient::Value b{5};

    EXPECT_EQ(a, b);
}

TEST(ValueEquality, DifferentIntegersAreNotEqual) {
    btclient::Value a{5};
    btclient::Value b{6};

    EXPECT_NE(a, b);
}

TEST(ValueEquality, EqualStringsAreEqual) {
    btclient::Value a{std::string("string")};
    btclient::Value b{std::string("string")};

    EXPECT_EQ(a, b);
}

TEST(ValueEquality, DifferentStringsAreNotEqual) {
    btclient::Value a{std::string("string1")};
    btclient::Value b{std::string("string2")};

    EXPECT_NE(a, b);
}

TEST(ValueEquality, IntegerIsNotEqualToString) {
    btclient::Value a{5};
    btclient::Value b{std::string("5")};

    EXPECT_NE(a, b);
}

TEST(ValueEquality, EqualListsAreEqual) {
    btclient::Value a{btclient::List{
        btclient::Value{1},
        btclient::Value{std::string("a")}
    }};
    btclient::Value b{btclient::List{
        btclient::Value{1},
        btclient::Value{std::string("a")}
    }};

    EXPECT_EQ(a, b);
}

TEST(ValueEquality, DifferentListsAreNotEqual) {
    btclient::Value a{btclient::List{
        btclient::Value{1},
        btclient::Value{std::string("a")}
    }};
    btclient::Value b{btclient::List{
        btclient::Value{1},
        btclient::Value{std::string("b")}
    }};

    {
        SCOPED_TRACE("same length");
        EXPECT_NE(a, b);
    }

    {
        SCOPED_TRACE("different length");
        b.data = btclient::List{btclient::Value{1}};
        EXPECT_NE(a, b);
    }
}

TEST(ValueEquality, EmptyListIsNotEqualToEmptyDict) {
    btclient::Value a{btclient::List{}};
    btclient::Value b{btclient::Dict{}};

    EXPECT_NE(a, b);
}

TEST(ValueEquality, EqualDictsAreEqual) {
    btclient::Value a{btclient::Dict{{"a", btclient::Value{1}}}};
    btclient::Value b{btclient::Dict{{"a", btclient::Value{1}}}};

    EXPECT_EQ(a, b);
}

TEST(ValueEquality, DifferentDictsAreNotEqual) {
    btclient::Value a{btclient::Dict{{"a", btclient::Value{1}}}};
    btclient::Value b{btclient::Dict{{"b", btclient::Value{1}}}};

    {
        SCOPED_TRACE("same length, different key");
        EXPECT_NE(a, b);
    }

    {
        SCOPED_TRACE("same length, different value");
        b.data = btclient::Dict{{"a", btclient::Value{2}}};
        EXPECT_NE(a, b);
    }

    {
        SCOPED_TRACE("different length");
        b.data = btclient::Dict{
            {"a", btclient::Value{1}},
            {"b", btclient::Value{1}}
        };
        EXPECT_NE(a, b);
    }
}

// Dict is a vector of pairs, so key order is significant. Valid bencode
// requires sorted keys, so decoded dictionaries have a single canonical order.
TEST(ValueEquality, DictKeyOrderMatters) {
    btclient::Value a{btclient::Dict{
        {"a", btclient::Value{1}},
        {"b", btclient::Value{2}}
    }};
    btclient::Value b{btclient::Dict{
        {"b", btclient::Value{2}},
        {"a", btclient::Value{1}}
    }};

    EXPECT_NE(a, b);
}

TEST(DecodeInteger, DecodesPositiveInteger) {
    btclient::Value a{42};
    btclient::Value b = btclient::decode("i42e");

    EXPECT_EQ(a, b);
}

TEST(DecodeInteger, DecodesZero) {
    btclient::Value a{0};
    btclient::Value b = btclient::decode("i0e");

    EXPECT_EQ(a, b);
}

TEST(DecodeInteger, DecodesNegativeInteger) {
    btclient::Value a{-7};
    btclient::Value b = btclient::decode("i-7e");

    EXPECT_EQ(a, b);
}

TEST(DecodeInteger, RejectsMissingTerminator) {
    EXPECT_THROW(static_cast<void>(btclient::decode("i7")), btclient::DecodeError);
}

TEST(DecodeInteger, RejectsEmptyInteger) {
    EXPECT_THROW(static_cast<void>(btclient::decode("ie")), btclient::DecodeError);
}

TEST(DecodeInteger, RejectsLoneMinusSign) {
    EXPECT_THROW(static_cast<void>(btclient::decode("i-e")), btclient::DecodeError);
}

TEST(DecodeInteger, RejectsPlusSign) {
    EXPECT_THROW(static_cast<void>(btclient::decode("i+5e")), btclient::DecodeError);
}

// One past the maximum value of std::int64_t (9223372036854775807).
TEST(DecodeInteger, RejectsOverflow) {
    EXPECT_THROW(static_cast<void>(btclient::decode("i9223372036854775808e")),
                 btclient::DecodeError);
}
