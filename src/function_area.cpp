#include "function_area.h"

#include <cstring>

namespace function_area {
namespace {

struct SceneDefinition {
  Scene scene;
  const char* id;
  Presentation presentation;
};

constexpr SceneDefinition kScenes[] = {
    {Scene::Welcome, "welcome",
     {u8"\u6B22\u8FCE\u4F7F\u7528\u7535\u7EB8\u4FBF\u5229\u8D34!", "^_^"}},
    {Scene::DueSoon, "due_soon",
     {u8"\u4E34\u8FD1\u4EFB\u52A1", ">_<"}},
    {Scene::DueCheck, "due_check",
     {u8"\u5230\u671F\u786E\u8BA4", ">_<"}},
    {Scene::NextAction, "next_action",
     {u8"\u4E0B\u4E00\u6B65\u5EFA\u8BAE", "^_^"}},
    {Scene::Summary, "summary",
     {u8"\u6BCF\u65E5\u603B\u7ED3", "^-^"}},
    {Scene::Warm, "warm",
     {u8"\u6E29\u99A8\u4E92\u52A8", "^_^"}},
};

constexpr size_t kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

}  // namespace

bool isValid(Scene scene) {
  for (size_t index = 0; index < kSceneCount; ++index) {
    if (scene == kScenes[index].scene) return true;
  }
  return false;
}

const char* sceneId(Scene scene) {
  for (size_t index = 0; index < kSceneCount; ++index) {
    if (scene == kScenes[index].scene) return kScenes[index].id;
  }
  return kScenes[0].id;
}

bool sceneFromId(const char* id, Scene& scene) {
  if (id == nullptr) return false;
  for (size_t index = 0; index < kSceneCount; ++index) {
    if (strcmp(id, kScenes[index].id) == 0) {
      scene = kScenes[index].scene;
      return true;
    }
  }
  return false;
}

const Presentation& presentationFor(Scene scene) {
  const char* id = sceneId(scene);
  for (size_t index = 0; index < kSceneCount; ++index) {
    if (strcmp(id, kScenes[index].id) == 0) return kScenes[index].presentation;
  }
  return kScenes[0].presentation;
}

}  // namespace function_area
