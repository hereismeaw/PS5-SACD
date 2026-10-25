"use strict";

const audio = document.querySelector("#audioElement");
const connection = document.querySelector("#connectionStatus");
const connectionLabel = document.querySelector("#connectionLabel");
const trackList = document.querySelector("#trackList");
const queueList = document.querySelector("#queueList");
const emptyState = document.querySelector("#emptyState");
const toast = document.querySelector("#toast");
const seekSlider = document.querySelector("#seekSlider");

let tracks = [];
let activeIndex = -1;
let filterText = "";
let shuffleEnabled = false;
let repeatMode = 0;
let isSeeking = false;
let lastToastTimer = 0;
let lastGamepadButtons = [];

const durations = new Map();
const coverColors = [
  "linear-gradient(145deg, #95e5d8, #35466a 54%, #b57cc1)",
  "linear-gradient(145deg, #e9a08d, #553e63 55%, #6983a0)",
  "linear-gradient(145deg, #aea3f2, #353955 57%, #d78cae)",
  "linear-gradient(145deg, #e5cb89, #55516e 53%, #6fb5a8)",
  "linear-gradient(145deg, #8ab8dd, #34354f 55%, #d49a70)",
];

function setConnection(state, label) {
  connection.classList.remove("connected", "error");
  if (state) connection.classList.add(state);
  const sourceCard = document.querySelector(".source-card");
  sourceCard.classList.toggle("connected", state === "connected");
  sourceCard.classList.toggle("error", state === "error");
  connectionLabel.textContent = label;
}

function showToast(message) {
  toast.textContent = message;
  toast.classList.add("visible");
  window.clearTimeout(lastToastTimer);
  lastToastTimer = window.setTimeout(() => toast.classList.remove("visible"), 2800);
}

function formatTime(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return "0:00";
  const wholeSeconds = Math.floor(seconds);
  const minutes = Math.floor(wholeSeconds / 60);
  return `${minutes}:${String(wholeSeconds % 60).padStart(2, "0")}`;
}

function safeText(value) {
  return String(value ?? "").replace(/[&<>"']/g, (character) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[character]);
}

function visibleTracks() {
  const query = filterText.trim().toLocaleLowerCase();
  if (!query) return tracks.map((track, index) => ({ track, index }));
  return tracks.flatMap((track, index) => {
    const haystack = `${track.title} ${track.artist} ${track.album ?? ""}`.toLocaleLowerCase();
    return haystack.includes(query) ? [{ track, index }] : [];
  });
}

function renderTracks() {
  const rows = visibleTracks();
  trackList.replaceChildren();
  document.querySelector("#trackCount").textContent = tracks.length
    ? String(tracks.length).padStart(2, "0")
    : "—";
  document.querySelector("#libraryCount").textContent = tracks.length
    ? `${tracks.length} ${tracks.length === 1 ? "record" : "records"}`
    : "No records yet";
  document.querySelector("#playCollection").disabled = tracks.length === 0;

  if (tracks.length === 0) {
    emptyState.classList.add("visible");
    document.querySelector("#emptyTitle").textContent = "Your library is standing by.";
    document.querySelector("#emptyMessage").textContent = "Add PCM WAV files to the shared folder on your computer, then refresh this room.";
  } else if (rows.length === 0) {
    emptyState.classList.add("visible");
    document.querySelector("#emptyTitle").textContent = "No matches in this room.";
    document.querySelector("#emptyMessage").textContent = "Try another title, artist, or album name.";
  } else {
    emptyState.classList.remove("visible");
  }

  for (const { track, index } of rows) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `track-row${activeIndex === index ? " selected" : ""}${activeIndex === index && !audio.paused ? " playing" : ""}`;
    button.setAttribute("aria-label", `Play ${track.title} by ${track.artist}`);
    const duration = durations.get(track.id);
    const isActive = activeIndex === index;
    button.innerHTML = `
      <span class="track-number">${isActive && !audio.paused ? '<span class="playing-bars"><i></i><i></i><i></i></span>' : String(index + 1).padStart(2, "0")}</span>
      <span class="track-main">
        <span class="track-cover" style="background:${coverColors[index % coverColors.length]}"></span>
        <span class="track-name"><strong>${safeText(track.title)}</strong><small>${safeText(track.artist || "Unknown artist")}</small></span>
      </span>
      <span class="track-format"><i class="format-dot"></i>PCM · WAV</span>
      <span class="track-duration">${duration ? formatTime(duration) : "—:—"}</span>`;
    button.addEventListener("click", () => selectTrack(index, true));
    trackList.append(button);
  }

  renderQueue();
}

function upcomingIndexes() {
  if (tracks.length < 2 || activeIndex < 0) return [];
  const indexes = [];
  for (let step = 1; step < tracks.length && indexes.length < 4; step += 1) {
    indexes.push((activeIndex + step) % tracks.length);
  }
  return indexes;
}

function renderQueue() {
  const indexes = upcomingIndexes();
  queueList.replaceChildren();
  document.querySelector("#queueCount").textContent = String(indexes.length);
  document.querySelector(".queue-note").style.display = indexes.length ? "none" : "flex";
  for (const index of indexes) {
    const track = tracks[index];
    const item = document.createElement("button");
    item.type = "button";
    item.className = "queue-row";
    item.setAttribute("aria-label", `Play next: ${track.title}`);
    item.innerHTML = `
      <span class="queue-cover" style="background:${coverColors[index % coverColors.length]}"></span>
      <span class="queue-copy"><strong>${safeText(track.title)}</strong><small>${safeText(track.artist || "Unknown artist")}</small></span>`;
    item.addEventListener("click", () => selectTrack(index, true));
    queueList.append(item);
  }
}

function paintPlayerState() {
  document.querySelector(".player-bar").classList.toggle("is-playing", !audio.paused);
  const toggle = document.querySelector("#togglePlayback");
  toggle.setAttribute("aria-label", audio.paused ? "Play" : "Pause");
  toggle.title = audio.paused ? "Play" : "Pause";
  renderTracks();
}

function selectTrack(index, autoplay) {
  if (!Number.isInteger(index) || index < 0 || index >= tracks.length) return;
  const changed = activeIndex !== index;
  activeIndex = index;
  const track = tracks[index];
  if (changed) {
    audio.pause();
    audio.src = track.url;
    audio.load();
    document.querySelector("#elapsedTime").textContent = "0:00";
    document.querySelector("#totalTime").textContent = "0:00";
    seekSlider.value = "0";
  }
  document.querySelector("#nowTitle").textContent = track.title;
  document.querySelector("#nowArtist").textContent = track.artist || "Unknown artist";
  document.querySelector("#miniCover").style.background = coverColors[index % coverColors.length];
  document.querySelector("#playingQuality").textContent = "PCM";
  renderTracks();
  if (autoplay) {
    const playRequest = audio.play();
    if (playRequest && typeof playRequest.catch === "function") {
      playRequest.catch(() => showToast("Playback was blocked. Press Play once more to start."));
    }
  }
}

function chooseNextIndex() {
  if (tracks.length === 0) return -1;
  if (shuffleEnabled && tracks.length > 1) {
    let candidate = activeIndex;
    while (candidate === activeIndex) candidate = Math.floor(Math.random() * tracks.length);
    return candidate;
  }
  if (activeIndex < 0) return 0;
  if (activeIndex + 1 < tracks.length) return activeIndex + 1;
  return repeatMode === 2 ? 0 : -1;
}

function nextTrack() {
  const index = chooseNextIndex();
  if (index >= 0) selectTrack(index, true);
  else audio.pause();
}

function previousTrack() {
  if (audio.currentTime > 3) {
    audio.currentTime = 0;
    return;
  }
  if (tracks.length > 0) selectTrack(activeIndex <= 0 ? tracks.length - 1 : activeIndex - 1, true);
}

async function refreshLibrary() {
  setConnection("", "CONNECTING");
  try {
    const response = await fetch("/api/library", { cache: "no-store" });
    if (!response.ok) throw new Error(`Library service returned ${response.status}`);
    const library = await response.json();
    if (!Array.isArray(library.tracks)) throw new Error("Library response is incomplete");
    const previousId = activeIndex >= 0 ? tracks[activeIndex]?.id : null;
    tracks = library.tracks.filter((track) => track && track.id && track.url && track.title);
    activeIndex = previousId ? tracks.findIndex((track) => track.id === previousId) : -1;
    if (activeIndex < 0 && audio.src) {
      audio.pause();
      audio.removeAttribute("src");
      audio.load();
      document.querySelector("#nowTitle").textContent = "Nothing playing";
      document.querySelector("#nowArtist").textContent = "Choose a WAV from your collection";
    }
    setConnection("connected", "LAN LIBRARY READY");
    renderTracks();
    if (tracks.length) showToast(`${tracks.length} WAV ${tracks.length === 1 ? "record" : "records"} ready`);
  } catch (error) {
    setConnection("error", "LIBRARY OFFLINE");
    if (!tracks.length) {
      renderTracks();
      document.querySelector("#emptyTitle").textContent = "The music room is offline.";
      document.querySelector("#emptyMessage").textContent = "Start the PS5-SACD library server on your computer, then refresh.";
    }
    showToast(`Could not reach the library: ${error.message}`);
  }
}

document.querySelector("#refreshLibrary").addEventListener("click", refreshLibrary);
document.querySelector("#playCollection").addEventListener("click", () => {
  if (tracks.length) selectTrack(activeIndex < 0 ? 0 : activeIndex, true);
});
document.querySelector("#togglePlayback").addEventListener("click", () => {
  if (activeIndex < 0) {
    if (tracks.length) selectTrack(0, true);
    return;
  }
  if (audio.paused) audio.play().catch(() => showToast("Press Play once more to start playback."));
  else audio.pause();
});
document.querySelector("#nextButton").addEventListener("click", nextTrack);
document.querySelector("#previousButton").addEventListener("click", previousTrack);
document.querySelector("#shuffleButton").addEventListener("click", (event) => {
  shuffleEnabled = !shuffleEnabled;
  event.currentTarget.setAttribute("aria-pressed", String(shuffleEnabled));
  showToast(shuffleEnabled ? "Shuffle is on" : "Shuffle is off");
});
document.querySelector("#repeatButton").addEventListener("click", (event) => {
  repeatMode = (repeatMode + 1) % 3;
  const label = ["Repeat off", "Repeat track", "Repeat collection"][repeatMode];
  event.currentTarget.setAttribute("aria-label", label);
  event.currentTarget.title = label;
  event.currentTarget.setAttribute("aria-pressed", String(repeatMode !== 0));
  showToast(label);
});
document.querySelector("#muteButton").addEventListener("click", (event) => {
  audio.muted = !audio.muted;
  event.currentTarget.setAttribute("aria-label", audio.muted ? "Unmute" : "Mute");
  event.currentTarget.style.color = audio.muted ? "var(--cyan)" : "";
});
document.querySelector("#volumeSlider").addEventListener("input", (event) => {
  audio.volume = Number(event.currentTarget.value);
  audio.muted = false;
});
document.querySelector("#searchTracks").addEventListener("input", (event) => {
  filterText = event.currentTarget.value;
  renderTracks();
});
document.querySelector("#libraryNav").addEventListener("click", () => {
  document.querySelector("#searchTracks").focus();
  document.querySelector("#library").scrollIntoView({ behavior: "smooth", block: "start" });
});
document.querySelector("#searchTracks").addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    event.currentTarget.value = "";
    filterText = "";
    renderTracks();
    event.currentTarget.blur();
  }
});

seekSlider.addEventListener("input", () => {
  isSeeking = true;
  if (Number.isFinite(audio.duration)) {
    document.querySelector("#elapsedTime").textContent = formatTime((Number(seekSlider.value) / 1000) * audio.duration);
  }
});
seekSlider.addEventListener("change", () => {
  if (Number.isFinite(audio.duration)) audio.currentTime = (Number(seekSlider.value) / 1000) * audio.duration;
  isSeeking = false;
});

audio.addEventListener("loadedmetadata", () => {
  if (activeIndex < 0) return;
  const duration = audio.duration;
  if (Number.isFinite(duration)) {
    durations.set(tracks[activeIndex].id, duration);
    document.querySelector("#totalTime").textContent = formatTime(duration);
    renderTracks();
  }
});
audio.addEventListener("timeupdate", () => {
  if (!Number.isFinite(audio.duration)) return;
  if (!isSeeking) seekSlider.value = String(Math.round((audio.currentTime / audio.duration) * 1000));
  document.querySelector("#elapsedTime").textContent = formatTime(audio.currentTime);
  document.querySelector("#totalTime").textContent = formatTime(audio.duration);
});
audio.addEventListener("play", paintPlayerState);
audio.addEventListener("pause", paintPlayerState);
audio.addEventListener("ended", () => {
  if (repeatMode === 1) {
    audio.currentTime = 0;
    audio.play().catch(() => {});
  } else {
    nextTrack();
  }
});
audio.addEventListener("error", () => {
  if (activeIndex >= 0) showToast("This WAV could not be played by the PS5 browser.");
});

document.addEventListener("keydown", (event) => {
  const tag = document.activeElement?.tagName;
  if (tag === "INPUT" || tag === "TEXTAREA" || event.altKey || event.ctrlKey || event.metaKey) return;
  if (event.code === "Space") {
    event.preventDefault();
    document.querySelector("#togglePlayback").click();
  } else if (event.key === "ArrowRight") {
    if (activeIndex >= 0 && Number.isFinite(audio.duration)) audio.currentTime = Math.min(audio.duration, audio.currentTime + 5);
  } else if (event.key === "ArrowLeft") {
    if (activeIndex >= 0) audio.currentTime = Math.max(0, audio.currentTime - 5);
  } else if (event.key === "/") {
    event.preventDefault();
    document.querySelector("#searchTracks").focus();
  }
});

function pollGamepad() {
  if (typeof navigator.getGamepads !== "function") return;
  const gamepads = navigator.getGamepads();
  const pad = Array.from(gamepads).find(Boolean);
  if (!pad) return;
  const pressed = (index) => Boolean(pad.buttons[index]?.pressed);
  const justPressed = (index) => pressed(index) && !lastGamepadButtons[index];
  if (justPressed(0)) document.querySelector("#togglePlayback").click();
  if (justPressed(14)) previousTrack();
  if (justPressed(15)) nextTrack();
  if (justPressed(12) && tracks.length) selectTrack(activeIndex <= 0 ? 0 : activeIndex - 1, false);
  if (justPressed(13) && tracks.length) selectTrack(activeIndex + 1 < tracks.length ? activeIndex + 1 : 0, false);
  lastGamepadButtons = pad.buttons.map((button) => Boolean(button.pressed));
}

audio.volume = 0.82;
window.setInterval(pollGamepad, 100);
refreshLibrary();
