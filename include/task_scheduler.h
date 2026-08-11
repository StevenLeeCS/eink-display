#pragma once

#include "function_area.h"

namespace task_scheduler {

using SceneCallback = void (*)(function_area::Scene scene);

void begin(SceneCallback callback);
void poll();
void taskStored();
void taskRemoved();
void completionChanged(bool completed);
void settingsChanged();

}  // namespace task_scheduler
