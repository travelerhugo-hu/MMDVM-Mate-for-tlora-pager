/**
 * @file    msgpack_lite.h
 * @brief   Minimal, allocation-free MessagePack reader/writer.
 *
 * Only the subset actually used by the BrandMeister "spotter" protocol is
 * implemented: arrays, unsigned/signed integers, float32/float64, str and bin.
 *
 * Design notes:
 *  - Zero heap usage. The reader is a cursor over a caller-owned buffer and
 *    hands back pointers into that buffer (no copies, no NUL terminators).
 *  - Every read is bounds-checked. A malformed frame can never walk off the
 *    end of the buffer - important because this data comes straight off the
 *    network into a fixed-size stack/PSRAM buffer.
 *  - No exceptions, no RTTI: every call returns bool for success.
 */
#pragma once

#include <Arduino.h>

class MsgPackReader {
public:
    MsgPackReader(const uint8_t *buf, size_t len)
        : _buf(buf), _len(buf ? len : 0), _pos(0) {}

    size_t remaining() const { return _pos <= _len ? _len - _pos : 0; }
    bool   atEnd()     const { return _pos >= _len; }

    /// Peek the next format byte without consuming it.
    bool peek(uint8_t &b) const
    {
        if (_pos >= _len) return false;
        b = _buf[_pos];
        return true;
    }

    bool readArrayHeader(uint32_t &count);
    bool readUInt(uint64_t &value);
    bool readInt(int64_t &value);
    bool readDouble(double &value);

    /// Zero-copy string. `ptr` is NOT NUL-terminated; use `len`.
    bool readStr(const uint8_t **ptr, uint32_t &len);

    /// Zero-copy binary blob.
    bool readBin(const uint8_t **ptr, uint32_t &len);

    /// Read either str or bin (the server is not always consistent).
    bool readBytes(const uint8_t **ptr, uint32_t &len);

    /// Skip exactly one value of any supported type.
    bool skipValue();

private:
    bool take(size_t n, const uint8_t **out)
    {
        if (n > remaining()) return false;
        *out = _buf + _pos;
        _pos += n;
        return true;
    }
    bool u8(uint8_t &v);
    bool be16(uint16_t &v);
    bool be32(uint32_t &v);
    bool be64(uint64_t &v);

    const uint8_t *_buf;
    size_t         _len;
    size_t         _pos;
};

class MsgPackWriter {
public:
    MsgPackWriter(uint8_t *buf, size_t cap) : _buf(buf), _cap(cap), _pos(0) {}

    bool writeArrayHeader(uint32_t count);
    bool writeUInt(uint32_t value);

    size_t         size() const { return _pos; }
    const uint8_t *data() const { return _buf; }
    bool           overflowed() const { return _overflow; }

private:
    bool put(uint8_t b)
    {
        if (_pos >= _cap) { _overflow = true; return false; }
        _buf[_pos++] = b;
        return true;
    }
    uint8_t *_buf;
    size_t   _cap;
    size_t   _pos;
    bool     _overflow = false;
};
