/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2017 Axel Waggershauser
    Copyright (C) 2018 Roman Lebedev

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

#pragma once

#include "decoders/RawDecoderException.h"       // for ThrowRDE
#include "decompressors/AbstractHuffmanTable.h" // for AbstractHuffmanTable...
#include "decompressors/BinaryHuffmanTree.h"    // IWYU pragma: export
#include "io/BitStream.h"                       // for BitStreamTraits
#include <algorithm>                            // for for_each
#include <cassert>                              // for assert
#include <initializer_list>                     // for initializer_list
#include <iterator>                             // for advance, next
#include <memory>                               // for unique_ptr, make_unique
#include <vector>                               // for vector, vector<>::co...

namespace rawspeed {

class HuffmanTableTree final : public AbstractHuffmanTable {
  using ValueType = decltype(codeValues)::value_type;

  BinaryHuffmanTree<ValueType> tree;

  bool fullDecode = true;
  bool fixDNGBug16 = false;

protected:
  template <typename BIT_STREAM>
  inline ValueType getValue(BIT_STREAM& bs) const {
    static_assert(BitStreamTraits<BIT_STREAM>::canUseWithHuffmanTable,
                  "This BitStream specialization is not marked as usable here");
    CodeSymbol partial;

    const auto* top = &(tree.root->getAsBranch());

    // Read bits until either find the code or detect the incorrect code
    for (partial.code = 0, partial.code_len = 1;; ++partial.code_len) {
      assert(partial.code_len <= 16);

      // Read one more bit
      const bool bit = bs.getBitsNoFill(1);

      partial.code <<= 1;
      partial.code |= bit;

      // What is the last bit, which we have just read?

      // NOTE: The order *IS* important! Left to right, zero to one!
      const auto& newNode = !bit ? top->zero : top->one;

      if (!newNode) {
        // Got nothing in this direction.
        ThrowRDE("bad Huffman code: %u (len: %u)", partial.code,
                 partial.code_len);
      }

      if (static_cast<decltype(tree)::Node::Type>(*newNode) ==
          decltype(tree)::Node::Type::Leaf) {
        // Ok, great, hit a Leaf. This is it.
        return newNode->getAsLeaf().value;
      }

      // Else, this is a branch, continue looking.
      top = &(newNode->getAsBranch());
    }

    // We have either returned the found symbol, or thrown on incorrect symbol.
    __builtin_unreachable();
  }

public:
  void setup(bool fullDecode_, bool fixDNGBug16_) {
    this->fullDecode = fullDecode_;
    this->fixDNGBug16 = fixDNGBug16_;

    assert(!nCodesPerLength.empty());
    assert(maxCodesCount() > 0);
    assert(codeValues.size() == maxCodesCount());

    auto currValue = codeValues.cbegin();
    for (auto codeLen = 1UL; codeLen < nCodesPerLength.size(); codeLen++) {
      const auto nCodesForCurrLen = nCodesPerLength[codeLen];

      auto nodes = tree.getAllVacantNodesAtDepth(codeLen);
      if (nodes.size() < nCodesForCurrLen) {
        ThrowRDE("Got too many (%u) codes for len %lu, can only have %zu codes",
                 nCodesForCurrLen, codeLen, nodes.size());
      }

      // Make first nCodesForCurrLen nodes Leafs
      std::for_each(nodes.cbegin(), std::next(nodes.cbegin(), nCodesForCurrLen),
                    [&currValue](auto* node) {
                      *node =
                          std::make_unique<decltype(tree)::Leaf>(*currValue);
                      std::advance(currValue, 1);
                    });
    }

    assert(codeValues.cend() == currValue);

    // And get rid of all the branches that do not lead to Leafs.
    // It is crucial to detect degenerate codes at the earliest.
    tree.pruneLeaflessBranches();
  }

  template <typename BIT_STREAM> inline int decodeLength(BIT_STREAM& bs) const {
    static_assert(BitStreamTraits<BIT_STREAM>::canUseWithHuffmanTable,
                  "This BitStream specialization is not marked as usable here");
    assert(!fullDecode);
    return decode<BIT_STREAM, false>(bs);
  }

  template <typename BIT_STREAM> inline int decodeNext(BIT_STREAM& bs) const {
    static_assert(BitStreamTraits<BIT_STREAM>::canUseWithHuffmanTable,
                  "This BitStream specialization is not marked as usable here");
    assert(fullDecode);
    return decode<BIT_STREAM, true>(bs);
  }

  // The bool template paraeter is to enable two versions:
  // one returning only the length of the of diff bits (see Hasselblad),
  // one to return the fully decoded diff.
  // All ifs depending on this bool will be optimized out by the compiler
  template <typename BIT_STREAM, bool FULL_DECODE>
  inline int decode(BIT_STREAM& bs) const {
    static_assert(BitStreamTraits<BIT_STREAM>::canUseWithHuffmanTable,
                  "This BitStream specialization is not marked as usable here");
    assert(FULL_DECODE == fullDecode);

    bs.fill(32);

    const auto codeValue = getValue(bs);

    const int diff_l = codeValue;

    if (!FULL_DECODE)
      return diff_l;

    if (diff_l == 16) {
      if (fixDNGBug16)
        bs.skipBitsNoFill(16);
      return -32768;
    }

    return diff_l ? extend(bs.getBitsNoFill(diff_l), diff_l) : 0;
  }
};

} // namespace rawspeed
