// spot5 - Reads and writes protobuf wire format without generated code.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Home's protobufs (casita, extended-metadata traits) have no .proto files in
// cspot, and only a handful of fields matter, so they are walked by field
// number instead (numbers recorded in SPOTIFY_MOBILE_RESEARCH.md §13 and
// checked against live responses).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace service::pb
{

struct Field
{
    std::uint32_t number = 0;
    std::uint32_t wire = 0;     // 0 varint, 1 fixed64, 2 bytes, 5 fixed32
    std::uint64_t varint = 0;   // wire 0
    std::string_view bytes;     // wire 2 (sub-message, string or bytes)
};

// Walks the fields of one message. next() returns false at the end, or when
// the data is malformed (then ok() is false too).
class Reader
{
  public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool next(Field &field)
    {
        if (pos_ >= data_.size())
            return false;
        std::uint64_t key = 0;
        if (!varint(key))
            return fail();
        field.number = static_cast<std::uint32_t>(key >> 3);
        field.wire = static_cast<std::uint32_t>(key & 7);
        field.bytes = {};
        field.varint = 0;
        switch (field.wire)
        {
        case 0:
            return varint(field.varint) || fail();
        case 1:
            return skip(8);
        case 2:
        {
            std::uint64_t size = 0;
            if (!varint(size) || size > data_.size() - pos_)
                return fail();
            field.bytes = data_.substr(pos_, static_cast<std::size_t>(size));
            pos_ += static_cast<std::size_t>(size);
            return true;
        }
        case 5:
            return skip(4);
        default:
            return fail();
        }
    }

    bool ok() const { return ok_; }

  private:
    bool varint(std::uint64_t &value)
    {
        value = 0;
        for (int shift = 0; shift < 64 && pos_ < data_.size(); shift += 7)
        {
            const auto byte = static_cast<std::uint8_t>(data_[pos_++]);
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if (byte < 0x80)
                return true;
        }
        return false;
    }
    bool skip(std::size_t count)
    {
        if (count > data_.size() - pos_)
            return fail();
        pos_ += count;
        return true;
    }
    bool fail()
    {
        ok_ = false;
        pos_ = data_.size();
        return false;
    }

    std::string_view data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// The first field `number` of the message (wire 2), or empty.
inline std::string_view first(std::string_view message, std::uint32_t number)
{
    Reader reader(message);
    Field field;
    while (reader.next(field))
        if (field.number == number && field.wire == 2)
            return field.bytes;
    return {};
}

// Follows a path of field numbers through nested messages: first(first(..)).
inline std::string_view at(std::string_view message, std::initializer_list<std::uint32_t> path)
{
    for (std::uint32_t number : path)
        message = first(message, number);
    return message;
}

// ---- writing (requests) ----

inline void put_varint(std::string &out, std::uint64_t value)
{
    while (value >= 0x80)
    {
        out += static_cast<char>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    out += static_cast<char>(value);
}

inline void put_varint_field(std::string &out, std::uint32_t number, std::uint64_t value)
{
    put_varint(out, (static_cast<std::uint64_t>(number) << 3) | 0);
    put_varint(out, value);
}

inline void put_bytes_field(std::string &out, std::uint32_t number, std::string_view value)
{
    put_varint(out, (static_cast<std::uint64_t>(number) << 3) | 2);
    put_varint(out, value.size());
    out.append(value);
}

} // namespace service::pb
