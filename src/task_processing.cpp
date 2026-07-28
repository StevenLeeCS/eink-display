#include "task_processing.h"

namespace task_processing {

TaskRecord fromRecognition(const String& recognizedText) {
  TaskRecord task;
  task.rawText = recognizedText;
  task.rawText.trim();
  return task;
}

String textForDisplay(const TaskRecord& task) {
  if (!kStructuredTasksEnabled || task.event.isEmpty()) return task.rawText;
  return task.event;
}

}  // namespace task_processing
