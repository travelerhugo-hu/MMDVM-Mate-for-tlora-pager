#include "msgpack_lite.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Primitive big-endian readers
// ---------------------------------------------------------------------------
bool MsgPackReader::u8(uint8_t &v)
{
    const uint8_t *p;
    if (!take(1, &p)) return false;
    v = p[0];
    return true;
}

bool MsgPackReader::be16(uint16_t &v)
{
    const uint8_t *p;
    if (!take(2, &p)) return false;
    v = (uint16_t)((uint16_t)p[0] << 8 | p[1]);
    return true;
}

bool MsgPackReader::be32(uint32_t &v)
{
    const uint8_t *p;
    if (!take(4, &p)) return false;
    v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    return true;
}

bool MsgPackReader::be64(uint64_t &v)
{
    uint32_t hi, lo;
    if (!be32(hi) || !be32(lo)) return false;
    v = ((uint64_t)hi << 32) | lo;
    return true;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
bool MsgPackReader::readArrayHeader(uint32_t &count)
{
    uint8_t t;
    if (!u8(t)) return false;

    if ((t & 0xF0) == 0x90) {           // fixarray
        count = t & 0x0F;
        return true;
    }
    if (t == 0xDC) {                    // array16
        uint16_t n;
        if (!be16(n)) return false;
        count = n;
        return true;
    }
    if (t == 0xDD) {                    // array32
        return be32(count);
    }
    return false;
}

bool MsgPackReader::readUInt(uint64_t &value)
{
    uint8_t t;
    if (!u8(t)) return false;

    if (t <= 0x7F) { value = t; return true; }          // positive fixint

    switch (t) {
    case 0xCC: { uint8_t v;  if (!u8(v))   return false; value = v; return true; }
    case 0xCD: { uint16_t v; if (!be16(v)) return false; value = v; return true; }
    case 0xCE: { uint32_t v; if (!be32(v)) return false; value = v; return true; }
    case 0xCF: return be64(value);
    default:   return false;
    }
}

bool MsgPackReader::readInt(int64_t &value)
{
    uint8_t t;
    if (!peek(t)) return false;

    // negative fixint
    if (t >= 0xE0) {
        (void)u8(t);
        value = (int8_t)t;
        return true;
    }
    if (t == 0xD0) { (void)u8(t); uint8_t v;  if (!u8(v))   return false; value = (int8_t)v;  return true; }
    if (t == 0xD1) { (void)u8(t); uint16_t v; if (!be16(v)) return false; value = (int16_t)v; return true; }
    if (t == 0xD2) { (void)u8(t); uint32_t v; if (!be32(v)) return false; value = (int32_t)v; return true; }
    if (t == 0xD3) { (void)u8(t); uint64_t v; if (!be64(v)) return false; value = (int64_t)v; return true; }

    uint64_t uv;
    if (!readUInt(uv)) return false;
    value = (int64_t)uv;
    return true;
}

bool MsgPackReader::readDouble(double &value)
{
    uint8_t t;
    if (!u8(t)) return false;

    if (t == 0xCA) {                    // float32
        uint32_t bits;
        if (!be32(bits)) return false;
        float f;
        // memcpy avoids the strict-aliasing UB that a pointer cast would cause.
        memcpy(&f, &bits, sizeof(f));
        value = f;
        return true;
    }
    if (t == 0xCB) {                    // float64
        uint64_t bits;
        if (!be64(bits)) return false;
        double d;
        memcpy(&d, &bits, sizeof(d));
        value = d;
        return true;
    }

    // Integers are acceptable where a float is expected.
    _pos--;
    int64_t iv;
    if (!readInt(iv)) return false;
    value = (double)iv;
    return true;
}

bool MsgPackReader::readStr(const uint8_t **ptr, uint32_t &len)
{
    uint8_t t;
    if (!u8(t)) return false;

    uint32_t n;
    if ((t & 0xE0) == 0xA0)      n = t & 0x1F;                              // fixstr
    else if (t == 0xD9)        { uint8_t v;  if (!u8(v))   return false; n = v; }
    else if (t == 0xDA)        { uint16_t v; if (!be16(v)) return false; n = v; }
    else if (t == 0xDB)        { if (!be32(n)) return false; }
    else                         return false;

    len = n;
    return take(n, ptr);
}

bool MsgPackReader::readBin(const uint8_t **ptr, uint32_t &len)
{
    uint8_t t;
    if (!u8(t)) return false;

    uint32_t n;
    if (t == 0xC4)      { uint8_t v;  if (!u8(v))   return false; n = v; }
    else if (t == 0xC5) { uint16_t v; if (!be16(v)) return false; n = v; }
    else if (t == 0xC6) { if (!be32(n)) return false; }
    else                  return false;

    len = n;
    return take(n, ptr);
}

bool MsgPackReader::readBytes(const uint8_t **ptr, uint32_t &len)
{
    uint8_t t;
    if (!peek(t)) return false;

    if (t == 0xC4 || t == 0xC5 || t == 0xC6) return readBin(ptr, len);
    return readStr(ptr, len);
}

bool MsgPackReader::skipValue()
{
    uint8_t t;
    if (!peek(t)) return false;

    // nil / bool
    if (t == 0xC0 || t == 0xC2 || t == 0xC3) { (void)u8(t); return true; }

    // int family
    if (t <= 0x7F || t >= 0xE0 ||
        (t >= 0xCC && t <= 0xCF) || (t >= 0xD0 && t <= 0xD3)) {
        int64_t dummy;
        return readInt(dummy);
    }
    // float family
    if (t == 0xCA || t == 0xCB) {
        double dummy;
        return readDouble(dummy);
    }
    // str / bin
    if ((t & 0xE0) == 0xA0 || (t >= 0xD9 && t <= 0xDB) || (t >= 0xC4 && t <= 0xC6)) {
        const uint8_t *p;
        uint32_t n;
        return readBytes(&p, n);
    }
    // array
    if ((t & 0xF0) == 0x90 || t == 0xDC || t == 0xDD) {
        uint32_t n;
        if (!readArrayHeader(n)) return false;
        for (uint32_t i = 0; i < n; i++) {
            if (!skipValue()) return false;
        }
        return true;
    }
    // map
    if ((t & 0xF0) == 0x80 || t == 0xDE || t == 0xDF) {
        uint32_t n = 0;
        (void)u8(t);
        if ((t & 0xF0) == 0x80)      n = t & 0x0F;
        else if (t == 0xDE)        { uint16_t v; if (!be16(v)) return false; n = v; }
        else                       { if (!be32(n)) return false; }
        for (uint32_t i = 0; i < n * 2; i++) {
            if (!skipValue()) return false;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
bool MsgPackWriter::writeArrayHeader(uint32_t count)
{
    if (count < 16) return put((uint8_t)(0x90 | count));
    if (count < 0x10000) {
        return put(0xDC) && put((uint8_t)(count >> 8)) && put((uint8_t)count);
    }
    return put(0xDD) && put((uint8_t)(count >> 24)) && put((uint8_t)(count >> 16)) &&
           put((uint8_t)(count >> 8)) && put((uint8_t)count);
}

bool MsgPackWriter::writeUInt(uint32_t value)
{
    if (value < 0x80)    return put((uint8_t)value);
    if (value <= 0xFF)   return put(0xCC) && put((uint8_t)value);
    if (value <= 0xFFFF) return put(0xCD) && put((uint8_t)(value >> 8)) && put((uint8_t)value);
    return put(0xCE) && put((uint8_t)(value >> 24)) && put((uint8_t)(value >> 16)) &&
           put((uint8_t)(value >> 8)) && put((uint8_t)value);
}
