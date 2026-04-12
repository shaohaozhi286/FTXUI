// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <array>       // for array
#include <functional>  // for function
#include <sstream>     // for basic_istream, stringstream
#include <string>      // for string, allocator, getline
#include <utility>     // for move

#include "ftxui/dom/elements.hpp"  // for flexbox, Element, text, Elements, operator|, xflex, paragraph, paragraphAlignCenter, paragraphAlignJustify, paragraphAlignLeft, paragraphAlignRight
#include "ftxui/dom/flexbox_config.hpp"  // for FlexboxConfig, FlexboxConfig::JustifyContent, FlexboxConfig::JustifyContent::Center, FlexboxConfig::JustifyContent::FlexEnd, FlexboxConfig::JustifyContent::SpaceBetween
#include "ftxui/screen/string.hpp"  // for Utf8ToGlyphs, string_width

namespace ftxui {

namespace {
bool IsSpaceGlyph(const std::string& glyph) {
  return glyph == " " || glyph == "\t";
}

bool IsNarrowGlyph(const std::string& glyph) {
  return string_width(glyph) == 1;
}

bool IsOpeningCjkPunctuation(const std::string& glyph) {
  static constexpr std::array<std::string_view, 8> kOpening = {
      "（", "《", "「", "【", "‘", "“", "〈", "『"};
  for (const auto& candidate : kOpening) {
    if (glyph == candidate) {
      return true;
    }
  }
  return false;
}

bool IsClosingCjkPunctuation(const std::string& glyph) {
  static constexpr std::array<std::string_view, 15> kClosing = {
      "，", "。", "！", "？", "；", "：", "、", "）",
      "》", "」", "】", "’", "”", "〉", "』"};
  for (const auto& candidate : kClosing) {
    if (glyph == candidate) {
      return true;
    }
  }
  return false;
}

void FlushAsciiRun(std::string* ascii_run,
                   std::string* pending_prefix,
                   std::vector<std::string>* output) {
  if (ascii_run->empty()) {
    return;
  }

  std::string token = std::move(*ascii_run);
  ascii_run->clear();
  if (!pending_prefix->empty()) {
    token = std::move(*pending_prefix) + token;
    pending_prefix->clear();
  }
  output->push_back(std::move(token));
}

Elements Split(const std::string& the_text) {
  std::vector<std::string> tokens;
  std::string ascii_run;
  std::string pending_prefix;

  for (const auto& glyph : Utf8ToGlyphs(the_text)) {
    if (glyph.empty()) {
      continue;
    }

    if (IsSpaceGlyph(glyph)) {
      FlushAsciiRun(&ascii_run, &pending_prefix, &tokens);
      if (!tokens.empty()) {
        tokens.back() += " ";
      }
      continue;
    }

    if (IsOpeningCjkPunctuation(glyph)) {
      FlushAsciiRun(&ascii_run, &pending_prefix, &tokens);
      pending_prefix += glyph;
      continue;
    }

    if (IsClosingCjkPunctuation(glyph)) {
      FlushAsciiRun(&ascii_run, &pending_prefix, &tokens);
      if (!tokens.empty()) {
        tokens.back() += glyph;
      } else if (!pending_prefix.empty()) {
        pending_prefix += glyph;
      } else {
        tokens.push_back(glyph);
      }
      continue;
    }

    if (IsNarrowGlyph(glyph)) {
      ascii_run += glyph;
      continue;
    }

    FlushAsciiRun(&ascii_run, &pending_prefix, &tokens);
    std::string token = glyph;
    if (!pending_prefix.empty()) {
      token = std::move(pending_prefix) + token;
      pending_prefix.clear();
    }
    tokens.push_back(std::move(token));
  }

  FlushAsciiRun(&ascii_run, &pending_prefix, &tokens);
  if (!pending_prefix.empty()) {
    if (!tokens.empty()) {
      tokens.back() += pending_prefix;
    } else {
      tokens.push_back(std::move(pending_prefix));
    }
  }

  Elements output;
  output.reserve(tokens.size());
  for (auto& token : tokens) {
    output.push_back(text(std::move(token)));
  }

  return output;
}

Element Split(const std::string& paragraph,
              const std::function<Element(std::string)>& f) {
  Elements output;
  std::stringstream ss(paragraph);
  std::string line;
  while (std::getline(ss, line, '\n')) {
    output.push_back(f(line));
  }
  return vbox(std::move(output));
}

}  // namespace

/// @brief Return an element drawing the paragraph on multiple lines.
/// @ingroup dom
/// @see flexbox.
Element paragraph(std::string_view the_text) {
  return paragraphAlignLeft(the_text);
}

/// @brief Return an element drawing the paragraph on multiple lines, aligned on
/// the left.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignLeft(std::string_view the_text) {
  return Split(std::string(the_text), [](const std::string& line) {
    static const auto config = FlexboxConfig().SetGap(0, 0);
    return flexbox(Split(line), config);
  });
};

/// @brief Return an element drawing the paragraph on multiple lines, aligned on
/// the right.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignRight(std::string_view the_text) {
  return Split(std::string(the_text), [](const std::string& line) {
    static const auto config = FlexboxConfig().SetGap(0, 0).Set(
        FlexboxConfig::JustifyContent::FlexEnd);
    return flexbox(Split(line), config);
  });
}

/// @brief Return an element drawing the paragraph on multiple lines, aligned on
/// the center.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignCenter(std::string_view the_text) {
  return Split(std::string(the_text), [](const std::string& line) {
    static const auto config =
        FlexboxConfig().SetGap(0, 0).Set(FlexboxConfig::JustifyContent::Center);
    return flexbox(Split(line), config);
  });
}

/// @brief Return an element drawing the paragraph on multiple lines, aligned
/// using a justified alignment.
/// the center.
/// @ingroup dom
/// @see flexbox.
Element paragraphAlignJustify(std::string_view the_text) {
  return Split(std::string(the_text), [](const std::string& line) {
    static const auto config = FlexboxConfig().SetGap(0, 0).Set(
        FlexboxConfig::JustifyContent::SpaceBetween);
    Elements words = Split(line);
    words.push_back(text("") | xflex);
    return flexbox(std::move(words), config);
  });
}

}  // namespace ftxui
