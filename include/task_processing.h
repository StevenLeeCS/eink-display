#pragma once

#include <Arduino.h>

namespace task_processing {

// The data boundary is present for the next development step, but structured
// task processing remains disabled until its cloud implementation is tested.
constexpr bool kStructuredTasksEnabled = false;

struct TaskRecord {
  String rawText;
  String time;
  String place;
  String person;
  String event;
};

TaskRecord fromRecognition(const String& recognizedText);
String textForDisplay(const TaskRecord& task);

}  // namespace task_processing
