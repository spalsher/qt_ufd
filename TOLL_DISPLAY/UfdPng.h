#pragma once
/* ======================================================================
   UfdPng  -  reads "UFD PNG" files with a few hundred bytes of RAM

   A UFD PNG is a completely normal PNG file (it opens in every browser),
   saved WITHOUT compression: 8-bit RGB or RGBA, no interlacing, row filter
   "none". UFD Image Studio saves this with "Save for UFD server".

   The file is read as it arrives from the network, every part is checked
   (PNG CRC of each chunk + zlib Adler-32 of the picture data), and each
   row is handed over as 64 RGB565 pixels. Nothing big is kept in RAM.
   ====================================================================== */
#include <stdint.h>
#include <string.h>

class UfdPngSource { public: virtual int read(uint8_t *buf, int n) = 0; };   // 0 = no more data
class UfdPngSink   { public: virtual bool row(int y, const uint16_t *px) = 0; }; // 64 pixels

enum UfdPngResult {
  UFDPNG_OK = 0, UFDPNG_NOT_PNG, UFDPNG_BAD_SIZE, UFDPNG_COMPRESSED, UFDPNG_FORMAT,
  UFDPNG_DAMAGED, UFDPNG_INCOMPLETE, UFDPNG_WRITE
};

inline const char *ufdPngText(int r)
{
  switch (r)
  {
    case UFDPNG_OK:         return "ok";
    case UFDPNG_NOT_PNG:    return "not a PNG file";
    case UFDPNG_BAD_SIZE:   return "wrong size";
    case UFDPNG_COMPRESSED: return "normal (compressed) PNG - save it with UFD Image Studio: Save for UFD server";
    case UFDPNG_FORMAT:     return "unsupported PNG type - save it with UFD Image Studio";
    case UFDPNG_DAMAGED:    return "damaged file (checksum wrong)";
    case UFDPNG_INCOMPLETE: return "download incomplete";
    default:                return "storage error";
  }
}

class UfdPngReader
{
public:
  int width = 0, height = 0;

  // expectH: 64 or 96. A 64-high slot also accepts 64x96 (its top 64 rows are used).
  int read(UfdPngSource &src, int expectH, UfdPngSink &sink)
  {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t b[8];
    if (!exact(src, b, 8)) return UFDPNG_INCOMPLETE;
    if (memcmp(b, sig, 8)) return UFDPNG_NOT_PNG;
    _src = &src; _sink = &sink; _expectH = expectH;
    _zstate = Z_HDR; _need = 2; _have = 0; _rowPos = 0; _y = 0; _adlerA = 1; _adlerB = 0;
    bool haveHdr = false;

    for (;;)
    {
      if (!exact(src, b, 8)) return UFDPNG_INCOMPLETE;
      uint32_t len = be32(b);
      uint32_t crc = crcUpdate(0xFFFFFFFF, b + 4, 4);
      bool ihdr = !memcmp(b + 4, "IHDR", 4), idat = !memcmp(b + 4, "IDAT", 4), iend = !memcmp(b + 4, "IEND", 4);
      if (!haveHdr && !ihdr) return UFDPNG_NOT_PNG;

      // chunk data in small pieces
      uint8_t buf[64];
      uint8_t hdr[13]; uint32_t hdrGot = 0;
      while (len)
      {
        int n = len > sizeof(buf) ? sizeof(buf) : (int)len;
        if (!exact(src, buf, n)) return UFDPNG_INCOMPLETE;
        crc = crcUpdate(crc, buf, n);
        if (ihdr) { for (int i = 0; i < n && hdrGot < 13; i++) hdr[hdrGot++] = buf[i]; }
        if (idat) { int r = inflateStored(buf, n); if (r != UFDPNG_OK) return r; }
        len -= n;
      }
      if (!exact(src, b, 4)) return UFDPNG_INCOMPLETE;
      if (be32(b) != (crc ^ 0xFFFFFFFF)) return UFDPNG_DAMAGED;

      if (ihdr)
      {
        if (hdrGot < 13) return UFDPNG_NOT_PNG;
        width = be32(hdr); height = be32(hdr + 4);
        if (width != 64 || !(height == expectH || (expectH == 64 && height == 96))) return UFDPNG_BAD_SIZE;
        if (hdr[8] != 8 || (hdr[9] != 2 && hdr[9] != 6) || hdr[10] || hdr[11] || hdr[12]) return UFDPNG_FORMAT;
        _bpp = hdr[9] == 6 ? 4 : 3;
        haveHdr = true;
      }
      if (iend) break;
    }
    if (_zstate != Z_DONE || _y != height) return UFDPNG_DAMAGED;
    return UFDPNG_OK;
  }

private:
  enum { Z_HDR, Z_BLOCK, Z_LEN, Z_DATA, Z_ADLER, Z_DONE };
  UfdPngSource *_src = nullptr; UfdPngSink *_sink = nullptr;
  int _expectH = 64, _bpp = 3;
  int _zstate = Z_HDR, _need = 2, _have = 0;
  uint8_t _acc[4];
  bool _final = false;
  uint32_t _blockLeft = 0;
  int _rowPos = 0, _y = 0;
  uint8_t _pix[4]; int _pixPos = 0;
  uint16_t _line[64];
  uint32_t _adlerA = 1, _adlerB = 0;

  static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

  static uint32_t crcUpdate(uint32_t c, const uint8_t *p, int n)
  {
    static const uint32_t T[16] = { 0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
      0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4,
      0xA00AE278, 0xBDBDF21C };
    for (int i = 0; i < n; i++) { c ^= p[i]; c = (c >> 4) ^ T[c & 15]; c = (c >> 4) ^ T[c & 15]; }
    return c;
  }

  bool exact(UfdPngSource &s, uint8_t *b, int n)
  {
    int got = 0;
    while (got < n) { int r = s.read(b + got, n - got); if (r <= 0) return false; got += r; }
    return true;
  }

  // picture byte (after zlib): row filter byte, then pixels
  int pixelByte(uint8_t v)
  {
    _adlerA = (_adlerA + v) % 65521; _adlerB = (_adlerB + _adlerA) % 65521;
    if (_y >= height) return UFDPNG_DAMAGED;
    if (_rowPos == 0)
    {
      if (v != 0) return UFDPNG_COMPRESSED;      // row filters are used by compressing savers
      _rowPos = 1; _pixPos = 0; return UFDPNG_OK;
    }
    _pix[_pixPos++] = v;
    if (_pixPos == _bpp)
    {
      int x = (_rowPos - 1) / _bpp;
      uint32_t r = _pix[0], g = _pix[1], bl = _pix[2];
      if (_bpp == 4) { uint32_t a = _pix[3]; r = r * a / 255; g = g * a / 255; bl = bl * a / 255; }  // over black
      _line[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
      _pixPos = 0;
    }
    _rowPos++;
    if (_rowPos == 1 + 64 * _bpp)
    {
      if (_y < _expectH && !_sink->row(_y, _line)) return UFDPNG_WRITE;
      _y++; _rowPos = 0;
    }
    return UFDPNG_OK;
  }

  // zlib stream made of "stored" blocks only (no compression)
  int inflateStored(const uint8_t *p, int n)
  {
    for (int i = 0; i < n; i++)
    {
      uint8_t v = p[i];
      switch (_zstate)
      {
        case Z_HDR:
          _acc[_have++] = v;
          if (_have == 2)
          {
            if ((_acc[0] & 0x0F) != 8 || ((_acc[0] << 8) | _acc[1]) % 31 || (_acc[1] & 0x20)) return UFDPNG_FORMAT;
            _zstate = Z_BLOCK; _have = 0;
          }
          break;
        case Z_BLOCK:
          if (((v >> 1) & 3) != 0) return UFDPNG_COMPRESSED;   // block type 0 = stored
          _final = v & 1; _zstate = Z_LEN; _have = 0;
          break;
        case Z_LEN:
          _acc[_have++] = v;
          if (_have == 4)
          {
            uint16_t len = _acc[0] | (_acc[1] << 8), nlen = _acc[2] | (_acc[3] << 8);
            if ((uint16_t)~len != nlen) return UFDPNG_DAMAGED;
            _blockLeft = len; _have = 0;
            _zstate = len ? Z_DATA : (_final ? Z_ADLER : Z_BLOCK);
          }
          break;
        case Z_DATA:
        {
          int r = pixelByte(v); if (r != UFDPNG_OK) return r;
          if (--_blockLeft == 0) _zstate = _final ? Z_ADLER : Z_BLOCK;
          break;
        }
        case Z_ADLER:
          _acc[_have++] = v;
          if (_have == 4)
          {
            uint32_t want = be32(_acc);
            if (want != ((_adlerB << 16) | _adlerA)) return UFDPNG_DAMAGED;
            _zstate = Z_DONE;
          }
          break;
        default:
          break;   // bytes after the end of the stream are ignored
      }
    }
    return UFDPNG_OK;
  }
};
