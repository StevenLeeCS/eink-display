#pragma once

#include <cstdint>

namespace function_area {

enum class Scene : uint8_t {
  Welcome = 0,
  DueSoon = 1,
  DueCheck = 2,
  NextAction = 4,
  Summary = 7,
  Warm = 8,
};

struct Presentation {
  const char* message;
  const char* emoticon;
};

bool isValid(Scene scene);
const char* sceneId(Scene scene);
bool sceneFromId(const char* id, Scene& scene);
const Presentation& presentationFor(Scene scene);

}  // namespace function_area
