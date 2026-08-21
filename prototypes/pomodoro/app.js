const STATE_KEY = "eink-pomodoro-interaction-v1";
const HISTORY_KEY = "eink-pomodoro-history-v1";
const MAX_HISTORY = 10;
const DOUBLE_CLICK_MS = 320;
const SHORT_PRESS_MAX_MS = 500;
const VOICE_HOLD_MS = 600;
const SILENT_HOLD_MS = 3000;
const DISPLAY_STEP_SECONDS = 5 * 60;

const elements = {
  modeState: document.getElementById("mode-state"),
  displayName: document.getElementById("display-name"),
  displayMessage: document.getElementById("display-message"),
  displayEmoticon: document.getElementById("display-emoticon"),
  status: document.getElementById("timer-status"),
  readout: document.getElementById("timer-readout"),
  progress: document.getElementById("progress-fill"),
  voiceCommand: document.getElementById("voice-command"),
  hardwareButton: document.getElementById("hardware-button"),
  holdStatus: document.getElementById("hold-status"),
  focusMinutes: document.getElementById("focus-minutes"),
  breakMinutes: document.getElementById("break-minutes"),
  phaseLabel: document.getElementById("phase-label"),
  roundLabel: document.getElementById("round-label"),
  targetLabel: document.getElementById("target-label"),
  history: document.getElementById("history-list"),
  clearHistory: document.getElementById("clear-history"),
  toast: document.getElementById("toast"),
};

let state = loadState();
let tickHandle = null;
let toastHandle = null;
let pressStartedAt = 0;
let silentHoldHandle = null;
let holdFeedbackHandle = null;
let pendingSingleHandle = null;
let longActionHandled = false;

function defaultState() {
  return {
    modeActive: false,
    status: "paused",
    phase: "focus",
    round: 1,
    focusTargetSeconds: 25 * 60,
    breakTargetSeconds: 5 * 60,
    phaseElapsedSeconds: 0,
    totalFocusSeconds: 0,
    totalBreakSeconds: 0,
    completedFocusRounds: 0,
    startedAt: 0,
    sessionStartedAt: 0,
    displayStep: 0,
    targetDisplayed: false,
  };
}

function loadState() {
  try {
    const saved = JSON.parse(localStorage.getItem(STATE_KEY));
    const validStatus = saved && ["running", "paused"].includes(saved.status);
    const validPhase = saved && ["focus", "break"].includes(saved.phase);
    if (validStatus && validPhase) return { ...defaultState(), ...saved };
  } catch (error) {
    // Invalid browser state falls back to a clean timer.
  }
  return defaultState();
}

function saveState() {
  localStorage.setItem(STATE_KEY, JSON.stringify(state));
}

function phaseElapsedNow() {
  const currentRun = state.modeActive && state.status === "running"
    ? Math.floor((Date.now() - state.startedAt) / 1000)
    : 0;
  return Math.max(0, state.phaseElapsedSeconds + currentRun);
}

function phaseTarget() {
  return state.phase === "focus" ? state.focusTargetSeconds : state.breakTargetSeconds;
}

function commitCurrentRun() {
  state.phaseElapsedSeconds = phaseElapsedNow();
  state.startedAt = 0;
}

function commitCurrentPhase(countCompletedFocus = false) {
  commitCurrentRun();
  if (state.phase === "focus") {
    state.totalFocusSeconds += state.phaseElapsedSeconds;
    if (countCompletedFocus && state.phaseElapsedSeconds > 0) {
      state.completedFocusRounds += 1;
    }
  } else {
    state.totalBreakSeconds += state.phaseElapsedSeconds;
  }
}

function enterMode() {
  const focusTargetSeconds = state.focusTargetSeconds;
  const breakTargetSeconds = state.breakTargetSeconds;
  state = defaultState();
  state.modeActive = true;
  state.focusTargetSeconds = focusTargetSeconds;
  state.breakTargetSeconds = breakTargetSeconds;
  state.sessionStartedAt = Date.now();
  saveState();
  updateEinkDisplay("enter");
  showToast("已进入番茄钟模式");
  render();
}

function exitMode() {
  if (!state.modeActive) return;
  commitCurrentPhase(false);
  const endedAt = Date.now();
  const record = {
    id: endedAt,
    startedAt: state.sessionStartedAt || endedAt,
    endedAt,
    focusSeconds: state.totalFocusSeconds,
    breakSeconds: state.totalBreakSeconds,
    focusRounds: state.completedFocusRounds,
  };
  if (record.focusSeconds > 0 || record.breakSeconds > 0) saveHistoryRecord(record);

  const focusTargetSeconds = state.focusTargetSeconds;
  const breakTargetSeconds = state.breakTargetSeconds;
  state = defaultState();
  state.focusTargetSeconds = focusTargetSeconds;
  state.breakTargetSeconds = breakTargetSeconds;
  saveState();
  updateEinkDisplay("exit", record);
  showToast(record.focusSeconds > 0 ? `已记录 ${formatDuration(record.focusSeconds)} 专注` : "已退出番茄钟模式");
  render();
  renderHistory();
}

function toggleRunning() {
  if (!state.modeActive) return;
  if (state.status === "running") {
    commitCurrentRun();
    state.status = "paused";
  } else {
    state.startedAt = Date.now();
    state.status = "running";
  }
  saveState();
  updateEinkDisplay(state.status === "running" ? "start" : "pause");
  render();
}

function switchPhase() {
  if (!state.modeActive) return;
  const completedPhase = state.phase;
  const completedSeconds = phaseElapsedNow();
  commitCurrentPhase(true);
  if (state.phase === "focus") {
    state.phase = "break";
  } else {
    state.phase = "focus";
    state.round += 1;
  }
  state.status = "paused";
  state.phaseElapsedSeconds = 0;
  state.startedAt = 0;
  state.displayStep = 0;
  state.targetDisplayed = false;
  saveState();
  updateEinkDisplay("switch", { completedPhase, completedSeconds });
  showToast(state.phase === "focus" ? `第 ${state.round} 轮专注待开始` : "休息待开始");
  render();
}

function parseVoiceSettings(text) {
  const normalized = text.replace(/[，。；]/g, ",").replace(/\s+/g, "");
  const focus = parseMinutesAfterKeyword(normalized, "专注");
  const rest = parseMinutesAfterKeyword(normalized, "休息");
  if (focus === null && rest === null) return null;
  if (focus !== null && (focus < 1 || focus > 180)) return null;
  if (rest !== null && (rest < 1 || rest > 60)) return null;
  return { focus, rest };
}

function parseMinutesAfterKeyword(text, keyword) {
  const match = text.match(new RegExp(`${keyword}(?:时间)?(?:改成|设置为|设为|是|为)?([0-9一二两三四五六七八九十百]+)(?:分钟|分)`));
  if (!match) return null;
  return /^\d+$/.test(match[1]) ? Number(match[1]) : chineseNumber(match[1]);
}

function chineseNumber(text) {
  const digits = { 一: 1, 二: 2, 两: 2, 三: 3, 四: 4, 五: 5, 六: 6, 七: 7, 八: 8, 九: 9 };
  if (text === "十") return 10;
  if (text === "百") return 100;
  let total = 0;
  const hundred = text.indexOf("百");
  if (hundred >= 0) {
    total += (digits[text[hundred - 1]] || 1) * 100;
    text = text.slice(hundred + 1);
  }
  const ten = text.indexOf("十");
  if (ten >= 0) {
    total += (digits[text[ten - 1]] || 1) * 10;
    text = text.slice(ten + 1);
  }
  if (text.length > 0) total += digits[text[text.length - 1]] || 0;
  return total;
}

function applyVoiceSettings() {
  const parsed = parseVoiceSettings(elements.voiceCommand.value.trim());
  if (!parsed) {
    showToast("未识别到有效的专注或休息分钟", true);
    updateEinkDisplay("voice_error");
    return;
  }
  if (parsed.focus !== null) state.focusTargetSeconds = parsed.focus * 60;
  if (parsed.rest !== null) state.breakTargetSeconds = parsed.rest * 60;
  if (!state.modeActive) {
    enterMode();
  } else {
    saveState();
    updateEinkDisplay("settings");
    render();
  }
  elements.voiceCommand.value = "";
  showToast("计时时间已更新");
}

function handlePressStart(event) {
  if (event.button !== undefined && event.button !== 0) return;
  event.preventDefault();
  if (event.pointerId !== undefined) {
    elements.hardwareButton.setPointerCapture?.(event.pointerId);
  }
  pressStartedAt = Date.now();
  longActionHandled = false;
  elements.hardwareButton.classList.add("pressed");
  updateHoldFeedback();
  holdFeedbackHandle = window.setInterval(updateHoldFeedback, 100);
  if (elements.voiceCommand.value.trim() === "") {
    silentHoldHandle = window.setTimeout(() => {
      longActionHandled = true;
      clearPendingSingle();
      state.modeActive ? exitMode() : enterMode();
      elements.holdStatus.textContent = "松开按键";
    }, SILENT_HOLD_MS);
  }
}

function handlePressEnd(event) {
  if (pressStartedAt === 0) return;
  event.preventDefault();
  const heldMs = Date.now() - pressStartedAt;
  const hasVoice = elements.voiceCommand.value.trim() !== "";
  clearHoldTimers();
  elements.hardwareButton.classList.remove("pressed");
  elements.holdStatus.textContent = "按住测试";
  pressStartedAt = 0;
  if (longActionHandled) return;
  if (hasVoice && heldMs >= VOICE_HOLD_MS) {
    clearPendingSingle();
    applyVoiceSettings();
    return;
  }
  if (!hasVoice && heldMs > SHORT_PRESS_MAX_MS) {
    showToast("静音进入或退出需要按满 3 秒");
    return;
  }
  registerClickGesture();
}

function handlePressCancel(event) {
  if (pressStartedAt === 0) return;
  event.preventDefault();
  clearHoldTimers();
  elements.hardwareButton.classList.remove("pressed");
  elements.holdStatus.textContent = "按住测试";
  pressStartedAt = 0;
  longActionHandled = false;
}

function registerClickGesture() {
  if (pendingSingleHandle !== null) {
    clearPendingSingle();
    switchPhase();
    return;
  }
  pendingSingleHandle = window.setTimeout(() => {
    pendingSingleHandle = null;
    toggleRunning();
  }, DOUBLE_CLICK_MS);
}

function clearPendingSingle() {
  if (pendingSingleHandle === null) return;
  window.clearTimeout(pendingSingleHandle);
  pendingSingleHandle = null;
}

function updateHoldFeedback() {
  const elapsed = Date.now() - pressStartedAt;
  if (elements.voiceCommand.value.trim() !== "") {
    elements.holdStatus.textContent = elapsed >= VOICE_HOLD_MS ? "松开提交语音" : "模拟录音中";
    return;
  }
  const remaining = Math.max(0, SILENT_HOLD_MS - elapsed);
  elements.holdStatus.textContent = remaining > 0 ? `静音 ${Math.ceil(remaining / 1000)} 秒` : "松开按键";
}

function clearHoldTimers() {
  window.clearTimeout(silentHoldHandle);
  window.clearInterval(holdFeedbackHandle);
  silentHoldHandle = null;
  holdFeedbackHandle = null;
}

function checkDisplayRefresh() {
  if (!state.modeActive || state.status !== "running") return;
  const elapsed = phaseElapsedNow();
  const step = Math.floor(elapsed / DISPLAY_STEP_SECONDS);
  const reachedTarget = elapsed >= phaseTarget();
  if (reachedTarget && !state.targetDisplayed) {
    state.targetDisplayed = true;
    state.displayStep = step;
    saveState();
    updateEinkDisplay("target");
    return;
  }
  if (step > state.displayStep) {
    state.displayStep = step;
    saveState();
    updateEinkDisplay("periodic");
  }
}

function updateEinkDisplay(reason, detail = {}) {
  if (reason === "exit") {
    elements.modeState.textContent = "普通模式";
    elements.displayMessage.textContent = detail.focusSeconds > 0
      ? `本次专注${wholeMinutes(detail.focusSeconds)}分钟`
      : "欢迎使用电纸便利贴!";
    elements.displayEmoticon.textContent = detail.focusSeconds > 0 ? "^-^" : "^_^";
    return;
  }
  if (!state.modeActive) {
    elements.modeState.textContent = "普通模式";
    elements.displayMessage.textContent = "欢迎使用电纸便利贴!";
    elements.displayEmoticon.textContent = "^_^";
    return;
  }

  elements.modeState.textContent = "番茄钟模式";
  const elapsed = phaseElapsedNow();
  const targetMinutes = wholeMinutes(phaseTarget());
  if (reason === "enter" || reason === "settings") {
    elements.displayMessage.textContent = `专注${wholeMinutes(state.focusTargetSeconds)}分,休息${wholeMinutes(state.breakTargetSeconds)}分`;
    elements.displayEmoticon.textContent = "^_^";
  } else if (reason === "switch") {
    const label = detail.completedPhase === "focus" ? "专注" : "休息";
    const next = state.phase === "focus" ? "专注待开始" : "休息待开始";
    elements.displayMessage.textContent = `${label}${wholeMinutes(detail.completedSeconds)}分,${next}`;
    elements.displayEmoticon.textContent = "^-^";
  } else if (reason === "voice_error") {
    elements.displayMessage.textContent = "时间设置失败,请重试";
    elements.displayEmoticon.textContent = ">_<";
  } else if (state.status === "paused") {
    elements.displayMessage.textContent = `${phaseText()}${wholeMinutes(elapsed)}分,已暂停`;
    elements.displayEmoticon.textContent = "._.";
  } else if (elapsed >= phaseTarget()) {
    elements.displayMessage.textContent = `${phaseText()}${wholeMinutes(elapsed)}分,已达目标`;
    elements.displayEmoticon.textContent = "^-^";
  } else {
    elements.displayMessage.textContent = `第${state.round}轮 ${phaseText()}${wholeMinutes(elapsed)}/${targetMinutes}分`;
    elements.displayEmoticon.textContent = state.phase === "focus" ? ">_<" : "^_^";
  }
}

function render() {
  const elapsed = phaseElapsedNow();
  const target = phaseTarget();
  const reached = state.modeActive && elapsed >= target;
  elements.modeState.textContent = state.modeActive ? "番茄钟模式" : "普通模式";
  elements.readout.textContent = formatClock(elapsed);
  elements.status.textContent = !state.modeActive
    ? "静音长按 3 秒进入"
    : state.status === "paused"
      ? `${phaseText()}已暂停`
      : reached ? `已超过目标 ${formatClock(elapsed - target)}` : `${phaseText()}中`;
  elements.phaseLabel.textContent = state.modeActive ? phaseText() : "--";
  elements.roundLabel.textContent = state.modeActive ? String(state.round) : "--";
  elements.targetLabel.textContent = state.modeActive ? formatDuration(target) : "--";
  elements.focusMinutes.value = wholeMinutes(state.focusTargetSeconds);
  elements.breakMinutes.value = wholeMinutes(state.breakTargetSeconds);
  elements.progress.classList.toggle("overdue", reached);
  elements.progress.style.width = `${state.modeActive ? Math.min(100, elapsed / target * 100) : 0}%`;

  if (state.modeActive && state.status === "running" && tickHandle === null) {
    tickHandle = window.setInterval(() => {
      checkDisplayRefresh();
      render();
    }, 250);
  }
  if ((!state.modeActive || state.status !== "running") && tickHandle !== null) {
    window.clearInterval(tickHandle);
    tickHandle = null;
  }
}

function updateBackendTargets() {
  const focus = clampMinutes(elements.focusMinutes.value, 25, 180);
  const rest = clampMinutes(elements.breakMinutes.value, 5, 60);
  state.focusTargetSeconds = focus * 60;
  state.breakTargetSeconds = rest * 60;
  saveState();
  if (state.modeActive) updateEinkDisplay("settings");
  render();
}

function clampMinutes(value, fallback, maximum) {
  return Math.min(maximum, Math.max(1, Math.round(Number(value) || fallback)));
}

function phaseText() {
  return state.phase === "focus" ? "专注" : "休息";
}

function wholeMinutes(seconds) {
  return Math.floor(seconds / 60);
}

function formatClock(seconds) {
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor(seconds % 3600 / 60);
  const remainder = seconds % 60;
  const base = `${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`;
  return hours > 0 ? `${String(hours).padStart(2, "0")}:${base}` : base;
}

function formatDuration(seconds) {
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor(seconds % 3600 / 60);
  if (hours > 0) return `${hours}小时${minutes}分钟`;
  if (minutes > 0) return `${minutes}分钟`;
  return `${seconds}秒`;
}

function saveHistoryRecord(record) {
  const history = loadHistory();
  history.unshift(record);
  localStorage.setItem(HISTORY_KEY, JSON.stringify(history.slice(0, MAX_HISTORY)));
}

function loadHistory() {
  try {
    const history = JSON.parse(localStorage.getItem(HISTORY_KEY));
    return Array.isArray(history) ? history : [];
  } catch (error) {
    return [];
  }
}

function renderHistory() {
  const history = loadHistory();
  elements.history.textContent = "";
  if (history.length === 0) {
    elements.history.innerHTML = '<div class="empty-history">暂无计时记录</div>';
    return;
  }
  history.forEach(record => {
    const item = document.createElement("article");
    item.className = "history-item";
    const heading = document.createElement("strong");
    heading.textContent = new Date(record.endedAt).toLocaleString("zh-CN", { month: "numeric", day: "numeric", hour: "2-digit", minute: "2-digit" });
    const summary = document.createElement("span");
    summary.textContent = `专注 ${formatDuration(record.focusSeconds)} · 休息 ${formatDuration(record.breakSeconds)} · ${record.focusRounds} 轮`;
    item.append(heading, summary);
    elements.history.append(item);
  });
}

function clearHistory() {
  localStorage.removeItem(HISTORY_KEY);
  renderHistory();
  showToast("计时记录已清空");
}

function showToast(message, error = false) {
  elements.toast.textContent = message;
  elements.toast.className = `toast show${error ? " error" : ""}`;
  window.clearTimeout(toastHandle);
  toastHandle = window.setTimeout(() => { elements.toast.className = "toast"; }, 2400);
}

elements.hardwareButton.addEventListener("pointerdown", handlePressStart);
elements.hardwareButton.addEventListener("pointerup", handlePressEnd);
elements.hardwareButton.addEventListener("pointercancel", handlePressCancel);
elements.hardwareButton.addEventListener("contextmenu", event => event.preventDefault());
elements.hardwareButton.addEventListener("keydown", event => {
  if ((event.key === " " || event.key === "Enter") && pressStartedAt === 0) {
    handlePressStart(event);
  }
});
elements.hardwareButton.addEventListener("keyup", event => {
  if (event.key === " " || event.key === "Enter") handlePressEnd(event);
});
elements.focusMinutes.addEventListener("change", updateBackendTargets);
elements.breakMinutes.addEventListener("change", updateBackendTargets);
elements.clearHistory.addEventListener("click", clearHistory);

render();
renderHistory();
updateEinkDisplay(state.modeActive ? "restore" : "welcome");
