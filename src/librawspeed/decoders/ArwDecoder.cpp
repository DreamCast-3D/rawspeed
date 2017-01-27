/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "decoders/ArwDecoder.h"
#include "common/Common.h"                // for uint32, ushort16, uchar8
#include "common/Point.h"                 // for iPoint2D
#include "decoders/RawDecoderException.h" // for ThrowRDE
#include "io/BitPumpMSB.h"                // for BitPumpMSB
#include "io/BitPumpPlain.h"              // for BitPumpPlain, BitStream<>:...
#include "io/ByteStream.h"                // for ByteStream
#include "io/IOException.h"               // for IOException
#include "metadata/ColorFilterArray.h"    // for ::CFA_BLUE, ::CFA_GREEN
#include "tiff/TiffEntry.h"               // for TiffEntry
#include "tiff/TiffIFD.h"                 // for TiffIFD, TiffRootIFD
#include "tiff/TiffTag.h"                 // for ::MAKE, ::MODEL, ::DNGPRIV...
#include <cstdio>                         // for NULL
#include <exception>                      // for exception
#include <map>                            // for map, _Rb_tree_iterator
#include <string>                         // for string, operator==, basic_...
#include <vector>                         // for vector

using namespace std;

namespace RawSpeed {

ArwDecoder::ArwDecoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
  mShiftDownScale = 0;
  decoderVersion = 1;
}

ArwDecoder::~ArwDecoder() {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = nullptr;
}

RawImage ArwDecoder::decodeRawInternal() {
  TiffIFD *raw = nullptr;
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(STRIPOFFSETS);

  if (data.empty()) {
    TiffEntry *model = mRootIFD->getEntryRecursive(MODEL);

    if (model && model->getString() == "DSLR-A100") {
      // We've caught the elusive A100 in the wild, a transitional format
      // between the simple sanity of the MRW custom format and the wordly
      // wonderfullness of the Tiff-based ARW format, let's shoot from the hip
      data = mRootIFD->getIFDsWithTag(SUBIFDS);
      if (data.empty())
        ThrowRDE("ARW: A100 format, couldn't find offset");
      raw = data[0];
      uint32 off = raw->getEntry(SUBIFDS)->getInt();
      uint32 width = 3881;
      uint32 height = 2608;

      mRaw->dim = iPoint2D(width, height);
      mRaw->createData();
      ByteStream input(mFile, off);

      try {
        DecodeARW(input, width, height);
      } catch (IOException &e) {
        mRaw->setError(e.what());
        // Let's ignore it, it may have delivered somewhat useful data.
      }

      return mRaw;
    }

    if (hints.find("srf_format") != hints.end()) {
      data = mRootIFD->getIFDsWithTag(IMAGEWIDTH);
      if (data.empty())
        ThrowRDE("ARW: SRF format, couldn't find width/height");
      raw = data[0];

      uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
      uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
      uint32 len = width*height*2;

      // Constants taken from dcraw
      uint32 off = 862144;
      uint32 key_off = 200896;
      uint32 head_off = 164600;

      // Replicate the dcraw contortions to get the "decryption" key
      const uchar8 *keyData = mFile->getData(key_off, 1);
      uint32 offset = (*keyData) * 4;
      keyData = mFile->getData(key_off + offset, 4);
      uint32 key = get4BE(keyData, 0);
      uchar8 *head = mFile->getDataWrt(head_off, 40);
      SonyDecrypt((uint32 *) head, 10, key);
      for (int i=26; i-- > 22; )
        key = key << 8 | head[i];

      // "Decrypt" the whole image buffer in place
      uchar8 *image_data = mFile->getDataWrt(off, len);
      SonyDecrypt((uint32 *) image_data, len/4, key);

      // And now decode as a normal 16bit raw
      mRaw->dim = iPoint2D(width, height);
      mRaw->createData();
      ByteStream input(mFile, off, len);
      Decode16BitRawBEunpacked(input, width, height);

      return mRaw;
    }

    ThrowRDE("ARW Decoder: No image data found");
  }

  raw = data[0];
  int compression = raw->getEntry(COMPRESSION)->getInt();
  if (1 == compression) {
    try {
      DecodeUncompressed(raw);
    } catch (IOException &e) {
      mRaw->setError(e.what());
    }

    return mRaw;
  }

  if (32767 != compression)
    ThrowRDE("ARW Decoder: Unsupported compression");

  TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
  TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

  if (offsets->count != 1) {
    ThrowRDE("ARW Decoder: Multiple Strips found: %u", offsets->count);
  }
  if (counts->count != offsets->count) {
    ThrowRDE("ARW Decoder: Byte count number does not match strip size: count:%u, strips:%u ", counts->count, offsets->count);
  }
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 bitPerPixel = raw->getEntry(BITSPERSAMPLE)->getInt();

  // Sony E-550 marks compressed 8bpp ARW with 12 bit per pixel
  // this makes the compression detect it as a ARW v1.
  // This camera has however another MAKER entry, so we MAY be able
  // to detect it this way in the future.
  data = mRootIFD->getIFDsWithTag(MAKE);
  if (data.size() > 1) {
    for (auto &i : data) {
      string make = i->getEntry(MAKE)->getString();
      /* Check for maker "SONY" without spaces */
      if (make == "SONY")
        bitPerPixel = 8;
    }
  }

  bool arw1 = counts->getInt() * 8 != width * height * bitPerPixel;
  if (arw1)
    height += 8;

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  auto *curve = new ushort16[0x4001];
  TiffEntry *c = raw->getEntry(SONY_CURVE);
  uint32 sony_curve[] = { 0, 0, 0, 0, 0, 4095 };

  for (uint32 i = 0; i < 4; i++)
    sony_curve[i+1] = (c->getShort(i) >> 2) & 0xfff;

  for (uint32 i = 0; i < 0x4001; i++)
    curve[i] = i;

  for (uint32 i = 0; i < 5; i++)
    for (uint32 j = sony_curve[i] + 1; j <= sony_curve[i+1]; j++)
      curve[j] = curve[j-1] + (1 << i);

  if (!uncorrectedRawValues)
    mRaw->setTable(curve, 0x4000, true);

  uint32 c2 = counts->getInt();
  uint32 off = offsets->getInt();

  if (!mFile->isValid(off))
    ThrowRDE("Sony ARW decoder: Data offset after EOF, file probably truncated");

  if (!mFile->isValid(off, c2))
    c2 = mFile->getSize() - off;

  ByteStream input(mFile, off, c2);

  try {
    if (arw1)
      DecodeARW(input, width, height);
    else
      DecodeARW2(input, width, height, bitPerPixel);
  } catch (IOException &e) {
    mRaw->setError(e.what());
    // Let's ignore it, it may have delivered somewhat useful data.
  }

  // Set the table, if it should be needed later.
  if (uncorrectedRawValues) {
    mRaw->setTable(curve, 0x4000, false);
  } else {
    mRaw->setTable(nullptr);
  }

  delete[] curve;

  return mRaw;
}

void ArwDecoder::DecodeUncompressed(TiffIFD* raw) {
  uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
  uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
  uint32 off = raw->getEntry(STRIPOFFSETS)->getInt();
  uint32 c2 = raw->getEntry(STRIPBYTECOUNTS)->getInt();

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();
  ByteStream input(mFile, off, c2);

  if (hints.find("sr2_format") != hints.end())
    Decode14BitRawBEunpacked(input, width, height);
  else
    Decode16BitRawUnpacked(input, width, height);
}

void ArwDecoder::DecodeARW(ByteStream &input, uint32 w, uint32 h) {
  BitPumpMSB bits(input);
  uchar8* data = mRaw->getData();
  auto *dest = (ushort16 *)&data[0];
  uint32 pitch = mRaw->pitch / sizeof(ushort16);
  int sum = 0;
  for (uint32 x = w; x--;) {
    for (uint32 y = 0; y < h + 1; y += 2) {
      bits.checkPos();
      bits.fill();
      if (y == h) y = 1;
      uint32 len = 4 - bits.getBitsNoFill(2);
      if (len == 3 && bits.getBitsNoFill(1)) len = 0;
      if (len == 4)
        while (len < 17 && !bits.getBitsNoFill(1)) len++;
      int diff = bits.getBits(len);
      if (len && (diff & (1 << (len - 1))) == 0)
        diff -= (1 << len) - 1;
      sum += diff;
      _ASSERTE(!(sum >> 12));
      if (y < h) dest[x+y*pitch] = sum;
    }
  }
}

void ArwDecoder::DecodeARW2(ByteStream &input, uint32 w, uint32 h, uint32 bpp) {

  if (bpp == 8) {
    in = &input;
    this->startThreads();
    return;
  } // End bpp = 8

  if (bpp == 12) {
    if (input.getRemainSize() < (w * 3 / 2))
      ThrowRDE("Sony Decoder: Image data section too small, file probably truncated");

    if (input.getRemainSize() < (w*h*3 / 2))
      h = input.getRemainSize() / (w * 3 / 2) - 1;

    uchar8 *outData = mRaw->getData();
    uint32 pitch = mRaw->pitch;
    const uchar8 *inData = input.getData(input.getRemainSize());

    for (uint32 y = 0; y < h; y++) {
      auto *dest = (ushort16 *)&outData[y * pitch];
      for (uint32 x = 0 ; x < w; x += 2) {
        uint32 g1 = *inData++;
        uint32 g2 = *inData++;
        dest[x] = (g1 | ((g2 & 0xf) << 8));
        uint32 g3 = *inData++;
        dest[x+1] = ((g2 >> 4) | (g3 << 4));
      }
    }
    // Shift scales, since black and white are the same as compressed precision
    mShiftDownScale = 2;
    return;
  }
  ThrowRDE("Unsupported bit depth");
}

void ArwDecoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("ARW Support check: Model name found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void ArwDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  //Default
  int iso = 0;

  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("ARW Meta Decoder: Model name found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("ARW Decoder: Make name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  setMetaData(meta, make, model, "", iso);
  mRaw->whitePoint >>= mShiftDownScale;
  mRaw->blackLevel >>= mShiftDownScale;

  // Set the whitebalance
  if (model == "DSLR-A100") { // Handle the MRW style WB of the A100
    if (mRootIFD->hasEntryRecursive(DNGPRIVATEDATA)) {
      TiffEntry *priv = mRootIFD->getEntryRecursive(DNGPRIVATEDATA);
      const uchar8 *offdata = priv->getData(4);
      uint32 off = get4LE(offdata,0);
      uint32 length = mFile->getSize()-off;
      const unsigned char *dpd = mFile->getData(off, length);
      uint32 currpos = 8;
      while (currpos+20 < length) {
        uint32 tag = get4BE(dpd, currpos);
        uint32 len = get4LE(dpd, currpos + 4);
        if (tag == 0x574247) { /* WBG */
          ushort16 tmp[4];
          for(uint32 i=0; i<4; i++)
            tmp[i] = get2LE(dpd, currpos + 12 + i * 2);

          mRaw->metadata.wbCoeffs[0] = (float) tmp[0];
          mRaw->metadata.wbCoeffs[1] = (float) tmp[1];
          mRaw->metadata.wbCoeffs[2] = (float) tmp[3];
          break;
        }
        currpos += max(len + 8, 1u); // max(,1) to make sure we make progress
      }
    }
  } else { // Everything else but the A100
    try {
      GetWB();
    } catch (const std::exception& e) {
      mRaw->setError(e.what());
      // We caught an exception reading WB, just ignore it
    }
  }
}

void ArwDecoder::SonyDecrypt(uint32 *buffer, uint32 len, uint32 key) {
  uint32 pad[128];

  // Initialize the decryption pad from the key
  for (int p=0; p < 4; p++)
    pad[p] = key = key * 48828125 + 1;
  pad[3] = pad[3] << 1 | (pad[0]^pad[2]) >> 31;
  for (int p=4; p < 127; p++)
    pad[p] = (pad[p-4]^pad[p-2]) << 1 | (pad[p-3]^pad[p-1]) >> 31;
  for (int p=0; p < 127; p++)
    pad[p] = get4BE((uchar8 *) &pad[p],0);

  int p = 127;
  // Decrypt the buffer in place using the pad
  while (len--) {
    pad[p & 127] = pad[(p+1) & 127] ^ pad[(p+1+64) & 127];
    *buffer++ ^= pad[p & 127];
    p++;
  }
}

void ArwDecoder::GetWB() {
  // Set the whitebalance for all the modern ARW formats (everything after A100)
  if (mRootIFD->hasEntryRecursive(DNGPRIVATEDATA)) {
    TiffEntry *priv = mRootIFD->getEntryRecursive(DNGPRIVATEDATA);
    TiffRootIFD makerNoteIFD(priv->getRootIfdData(), priv->getInt());

    TiffEntry *sony_offset = makerNoteIFD.getEntryRecursive(SONY_OFFSET);
    TiffEntry *sony_length = makerNoteIFD.getEntryRecursive(SONY_LENGTH);
    TiffEntry *sony_key = makerNoteIFD.getEntryRecursive(SONY_KEY);
    if(!sony_offset || !sony_length || !sony_key || sony_key->count != 4)
      ThrowRDE("ARW: couldn't find the correct metadata for WB decoding");

    uint32 off = sony_offset->getInt();
    uint32 len = sony_length->getInt();
    const uchar8* data = sony_key->getData(4);
    uint32 key = get4LE(data,0);

    //TODO: replace ugly inplace decryption of (const) raw data
    auto *ifp_data = (uint32 *)mFile->getDataWrt(off, len);
    SonyDecrypt(ifp_data, len/4, key);

    TiffRootIFD encryptedIFD(priv->getRootIfdData(), off);

    if (encryptedIFD.hasEntry(SONYGRBGLEVELS)){
      TiffEntry *wb = encryptedIFD.getEntry(SONYGRBGLEVELS);
      if (wb->count != 4)
        ThrowRDE("ARW: WB has %d entries instead of 4", wb->count);
      mRaw->metadata.wbCoeffs[0] = wb->getFloat(1);
      mRaw->metadata.wbCoeffs[1] = wb->getFloat(0);
      mRaw->metadata.wbCoeffs[2] = wb->getFloat(2);
    } else if (encryptedIFD.hasEntry(SONYRGGBLEVELS)){
      TiffEntry *wb = encryptedIFD.getEntry(SONYRGGBLEVELS);
      if (wb->count != 4)
        ThrowRDE("ARW: WB has %d entries instead of 4", wb->count);
      mRaw->metadata.wbCoeffs[0] = wb->getFloat(0);
      mRaw->metadata.wbCoeffs[1] = wb->getFloat(1);
      mRaw->metadata.wbCoeffs[2] = wb->getFloat(3);
    }
  }
}

/* Since ARW2 compressed images have predictable offsets, we decode them threaded */

void ArwDecoder::decodeThreaded(RawDecoderThread * t) {
  uchar8* data = mRaw->getData();
  uint32 pitch = mRaw->pitch;
  int32 w = mRaw->dim.x;

  BitPumpPlain bits(*in);
  for (uint32 y = t->start_y; y < t->end_y; y++) {
    auto *dest = (ushort16 *)&data[y * pitch];
    // Realign
    bits.setBufferPosition(w*y);
    uint32 random = bits.peekBits(24);

    // Process 32 pixels (16x2) per loop.
    for (int32 x = 0; x < w - 30;) {
      bits.checkPos();
      int _max = bits.getBits(11);
      int _min = bits.getBits(11);
      int _imax = bits.getBits(4);
      int _imin = bits.getBits(4);
      int sh;
      for (sh = 0; sh < 4 && 0x80 << sh <= _max - _min; sh++);
      for (int i = 0; i < 16; i++) {
        int p;
        if (i == _imax) p = _max;
        else if (i == _imin) p = _min;
        else {
          p = (bits.getBits(7) << sh) + _min;
          if (p > 0x7ff)
            p = 0x7ff;
        }
        mRaw->setWithLookUp(p << 1, (uchar8*)&dest[x+i*2], &random);
      }
      x += x & 1 ? 31 : 1;  // Skip to next 32 pixels
    }
  }
}

} // namespace RawSpeed
