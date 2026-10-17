// Restricting recognition to a set of languages. Qwen3-ASR either detects the language among all of its own or is
// forced to one, so a set is enforced after the fact: a detection outside the set is recognized again, forced to the
// language of the set that the detected text's script points to.
#pragma once

#include <string>
#include <vector>

namespace asrd {

std::vector<std::string> parse_languages(const std::string & list);

std::string base_language(const std::string & tag);

bool language_in(const std::string & tag, const std::vector<std::string> & set);

std::string closest_language(const std::string & text, const std::vector<std::string> & set);

}  // namespace asrd
