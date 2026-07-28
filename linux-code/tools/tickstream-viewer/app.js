/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

"use strict";

const RECORD_BYTES = 32;
const SAMPLE_BYTES = 8;

const SIGNALS = [
  { key: "phi2", label: "PHI2", color: "#69d6a3" },
  { key: "r_w", label: "R/W", color: "#83b8ff" },
  { key: "ba", label: "BA", color: "#f2c15b" },
  { key: "irq", label: "/IRQ", color: "#ff8f8f" },
  { key: "dma", label: "/DMA", color: "#9cdcfe" },
  { key: "charen", label: "/CHAREN", color: "#56d364" },
  { key: "hiram", label: "/HIRAM", color: "#d29922" },
  { key: "loram", label: "/LORAM", color: "#bc8cff" },
  { key: "port01_chl_match", label: "$01=CHL", color: "#ff7b72" },
];

const state = {
  file: null,
  totalSamples: 0,
  sliceByteStart: 0,
  sliceByteEnd: 0,
  buffer: null,
  view: null,
  renderToken: 0,
  selectedSample: null,
};

const FILTER_SCAN_RECORDS = 65536;

const els = {
  fileInput: document.getElementById("fileInput"),
  startSample: document.getElementById("startSample"),
  sampleCount: document.getElementById("sampleCount"),
  addressFilter: document.getElementById("addressFilter"),
  reverseLanes: document.getElementById("reverseLanes"),
  prevButton: document.getElementById("prevButton"),
  nextButton: document.getElementById("nextButton"),
  applyButton: document.getElementById("applyButton"),
  fileName: document.getElementById("fileName"),
  fileSize: document.getElementById("fileSize"),
  sampleTotal: document.getElementById("sampleTotal"),
  windowInfo: document.getElementById("windowInfo"),
  waveform: document.getElementById("waveform"),
  sampleRows: document.getElementById("sampleRows"),
};

function bit(value, position) {
  return Number((value >> BigInt(position)) & 1n);
}

function field(value, shift, mask) {
  return Number((value >> BigInt(shift)) & BigInt(mask));
}

function hex(value, width) {
  return value.toString(16).toUpperCase().padStart(width, "0");
}

function formatBytes(bytes) {
  const units = ["B", "KB", "MB", "GB"];
  let value = bytes;
  let unit = 0;

  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }

  return `${value.toFixed(unit === 0 ? 0 : 2)} ${units[unit]}`;
}

function sampleByteOffset(index) {
  if (!els.reverseLanes.checked) {
    return index * SAMPLE_BYTES;
  }

  const record = Math.floor(index / 4);
  const lane = index & 3;
  return record * RECORD_BYTES + (3 - lane) * SAMPLE_BYTES;
}

function rawSample(index) {
  const offset = sampleByteOffset(index);
  const localOffset = offset - state.sliceByteStart;

  if (
    !state.view
    || offset < state.sliceByteStart
    || offset + SAMPLE_BYTES > state.sliceByteEnd
    || localOffset < 0
    || localOffset + SAMPLE_BYTES > state.buffer.byteLength
  ) {
    return 0n;
  }

  return state.view.getBigUint64(localOffset, true);
}

function decodeSample(index) {
  const raw = rawSample(index);

  return {
    index,
    raw,
    data: field(raw, 0, 0xff),
    addr: field(raw, 8, 0xffff),
    r_w: bit(raw, 24),
    ba: bit(raw, 25),
    phi2: bit(raw, 26),
    loram: bit(raw, 27),
    hiram: bit(raw, 28),
    charen: bit(raw, 29),
    cycle: field(raw, 30, 0xff),
    line: field(raw, 38, 0x1ff),
    tick: field(raw, 47, 0x3f),
    port01_chl_match: bit(raw, 53),
    dma: bit(raw, 54),
    irq: bit(raw, 55),
    frame: field(raw, 56, 0xff),
  };
}

function parseAddressFilter() {
  const text = els.addressFilter.value.trim();
  if (!text) {
    return null;
  }

  const normalized = text.replace(/^0x/i, "");
  const parts = normalized.split("-").map((part) => part.trim().replace(/^0x/i, ""));
  const start = Number.parseInt(parts[0], 16);
  const end = Number.parseInt(parts[1] || parts[0], 16);

  if (!Number.isFinite(start) || !Number.isFinite(end)) {
    return null;
  }

  return {
    start: Math.min(start, end) & 0xffff,
    end: Math.max(start, end) & 0xffff,
  };
}

function sampleFromRecord(view, recordOffset, recordIndex, lane) {
  const sampleOffset = recordOffset + (els.reverseLanes.checked ? (3 - lane) * SAMPLE_BYTES : lane * SAMPLE_BYTES);
  const raw = view.getBigUint64(sampleOffset, true);

  return {
    index: recordIndex * 4 + lane,
    raw,
    addr: field(raw, 8, 0xffff),
  };
}

function filterMatches(sample, filter) {
  return !filter || (sample.addr >= filter.start && sample.addr <= filter.end);
}

async function findMatchingSample(startIndex, filter) {
  if (!state.file || !filter || state.totalSamples === 0) {
    return null;
  }

  const totalRecords = Math.ceil(state.totalSamples / 4);
  const startRecord = Math.floor(startIndex / 4);
  const passes = [
    { first: startRecord, last: totalRecords },
    { first: 0, last: startRecord },
  ];

  for (const pass of passes) {
    for (let record = pass.first; record < pass.last; record += FILTER_SCAN_RECORDS) {
      const recordsToRead = Math.min(FILTER_SCAN_RECORDS, pass.last - record);
      const byteStart = record * RECORD_BYTES;
      const byteEnd = Math.min(state.file.size, byteStart + recordsToRead * RECORD_BYTES);
      const buffer = await state.file.slice(byteStart, byteEnd).arrayBuffer();
      const view = new DataView(buffer);
      const recordsRead = Math.floor(buffer.byteLength / RECORD_BYTES);

      for (let localRecord = 0; localRecord < recordsRead; localRecord++) {
        const recordIndex = record + localRecord;
        const recordOffset = localRecord * RECORD_BYTES;

        for (let lane = 0; lane < 4; lane++) {
          const sample = sampleFromRecord(view, recordOffset, recordIndex, lane);
          if (sample.index >= state.totalSamples) {
            break;
          }
          if (sample.index < startIndex && pass.first === startRecord) {
            continue;
          }
          if (filterMatches(sample, filter)) {
            return sample.index;
          }
        }
      }
    }
  }

  return null;
}

function getWindow() {
  const start = Math.max(0, Number.parseInt(els.startSample.value || "0", 10));
  const count = Math.max(16, Number.parseInt(els.sampleCount.value || "512", 10));
  const clampedStart = Math.min(start, Math.max(0, state.totalSamples - 1));
  const clampedCount = Math.min(count, Math.max(0, state.totalSamples - clampedStart));

  els.startSample.value = clampedStart;
  els.sampleCount.value = clampedCount || count;

  return { start: clampedStart, count: clampedCount };
}

async function ensureWindowLoaded(start, count) {
  if (!state.file || count === 0) {
    state.buffer = null;
    state.view = null;
    state.sliceByteStart = 0;
    state.sliceByteEnd = 0;
    return;
  }

  const firstByte = sampleByteOffset(start);
  const lastByte = sampleByteOffset(start + count - 1) + SAMPLE_BYTES;
  const sliceStart = Math.max(0, Math.floor(Math.min(firstByte, lastByte) / RECORD_BYTES) * RECORD_BYTES);
  const sliceEnd = Math.min(
    state.file.size,
    Math.ceil(Math.max(firstByte, lastByte) / RECORD_BYTES) * RECORD_BYTES
  );

  if (
    state.view
    && sliceStart >= state.sliceByteStart
    && sliceEnd <= state.sliceByteEnd
  ) {
    return;
  }

  state.sliceByteStart = sliceStart;
  state.sliceByteEnd = sliceEnd;
  state.buffer = await state.file.slice(sliceStart, sliceEnd).arrayBuffer();
  state.view = new DataView(state.buffer);
}

function drawWaveforms() {
  const canvas = els.waveform;
  const ctx = canvas.getContext("2d");
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.max(900, Math.floor(rect.width * dpr));
  canvas.height = Math.floor(720 * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  const width = canvas.width / dpr;
  const height = canvas.height / dpr;
  const left = 92;
  const right = 16;
  const top = 22;
  const laneHeight = 34;
  const busTop = top + SIGNALS.length * laneHeight + 24;
  const plotWidth = width - left - right;
  const { start, count } = getWindow();
  const filter = parseAddressFilter();

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#0c0f0e";
  ctx.fillRect(0, 0, width, height);

  ctx.font = "12px Cascadia Mono, Consolas, monospace";
  ctx.textBaseline = "middle";

  if (!state.file || !state.view || count === 0) {
    ctx.fillStyle = "#9bac9f";
    ctx.fillText("Open a capture file to draw waveforms.", 18, 28);
    return;
  }

  ctx.strokeStyle = "#25302b";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 8; i++) {
    const x = left + (plotWidth * i) / 8;
    ctx.beginPath();
    ctx.moveTo(x, top - 8);
    ctx.lineTo(x, height - 20);
    ctx.stroke();
  }

  SIGNALS.forEach((signal, lane) => {
    const y = top + lane * laneHeight;
    const highY = y + 7;
    const lowY = y + 24;

    ctx.fillStyle = "#9bac9f";
    ctx.fillText(signal.label, 12, y + laneHeight / 2);

    ctx.strokeStyle = "#24312b";
    ctx.beginPath();
    ctx.moveTo(left, highY);
    ctx.lineTo(width - right, highY);
    ctx.moveTo(left, lowY);
    ctx.lineTo(width - right, lowY);
    ctx.stroke();

    ctx.strokeStyle = signal.color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();

    let previousX = left;
    let previousY = null;
    for (let i = 0; i < count; i++) {
      const sample = decodeSample(start + i);
      const x = left + (count === 1 ? 0 : (plotWidth * i) / (count - 1));
      const yv = sample[signal.key] ? highY : lowY;

      if (previousY === null) {
        ctx.moveTo(x, yv);
      } else {
        ctx.lineTo(x, previousY);
        ctx.lineTo(x, yv);
      }

      previousX = x;
      previousY = yv;
    }
    ctx.lineTo(previousX, previousY);
    ctx.stroke();
    ctx.lineWidth = 1;
  });

  drawBusTrace(ctx, start, count, filter, left, plotWidth, busTop, width - right);
  drawSelectionMarker(ctx, start, count, left, plotWidth, top, height);
}

function drawSelectionMarker(ctx, start, count, left, plotWidth, top, height) {
  if (
    state.selectedSample === null
    || state.selectedSample < start
    || state.selectedSample >= start + count
  ) {
    return;
  }

  const offset = state.selectedSample - start;
  const x = left + (count === 1 ? 0 : (plotWidth * offset) / (count - 1));

  ctx.save();
  ctx.strokeStyle = "#ffffff";
  ctx.lineWidth = 1;
  ctx.setLineDash([5, 5]);
  ctx.beginPath();
  ctx.moveTo(x, top - 10);
  ctx.lineTo(x, height - 18);
  ctx.stroke();
  ctx.setLineDash([]);

  ctx.fillStyle = "#ffffff";
  ctx.fillRect(x - 2, top - 12, 4, 4);
  ctx.font = "12px Cascadia Mono, Consolas, monospace";
  ctx.textBaseline = "top";
  ctx.fillText(`#${state.selectedSample}`, Math.min(x + 6, left + plotWidth - 70), top - 16);
  ctx.restore();
}

function drawBusTrace(ctx, start, count, filter, left, plotWidth, y, rightEdge) {
  ctx.fillStyle = "#9bac9f";
  ctx.fillText("ADDR", 12, y + 18);
  ctx.fillText("DATA", 12, y + 50);
  ctx.fillText("TICK", 12, y + 82);

  let lastAddr = null;
  let lastData = null;
  let lastTick = null;
  let lastAddrLabelX = -Infinity;
  let lastDataLabelX = -Infinity;
  let lastTickLabelX = -Infinity;
  const busLabelSpacing = 44;

  for (let i = 0; i < count; i++) {
    const sample = decodeSample(start + i);
    const x = left + (count === 1 ? 0 : (plotWidth * i) / (count - 1));
    const inFilter = !filter || (sample.addr >= filter.start && sample.addr <= filter.end);

    if (filter && inFilter) {
      ctx.fillStyle = "rgba(105, 214, 163, 0.12)";
      ctx.fillRect(x, y - 4, Math.max(1, plotWidth / count), 100);
    }

    if (sample.addr !== lastAddr && x - lastAddrLabelX > busLabelSpacing) {
      drawBusLabel(ctx, hex(sample.addr, 4), x, y + 18, inFilter ? "#69d6a3" : "#edf4ef");
      lastAddr = sample.addr;
      lastAddrLabelX = x;
    }

    if (sample.data !== lastData && x - lastDataLabelX > busLabelSpacing && x + 24 < rightEdge) {
      drawBusLabel(ctx, hex(sample.data, 2), x, y + 50, "#f2c15b");
      lastData = sample.data;
      lastDataLabelX = x;
    }

    if (sample.tick !== lastTick && x - lastTickLabelX > busLabelSpacing && x + 24 < rightEdge) {
      drawBusLabel(ctx, String(sample.tick), x, y + 82, "#9bac9f");
      lastTick = sample.tick;
      lastTickLabelX = x;
    }
  }
}

function drawBusLabel(ctx, text, x, y, color) {
  const width = ctx.measureText(text).width + 4;

  ctx.fillStyle = "rgba(12, 15, 14, 0.82)";
  ctx.fillRect(x - 2, y - 8, width, 16);
  ctx.fillStyle = color;
  ctx.fillText(text, x, y);
}

function renderTable() {
  if (!state.file || !state.view) {
    return;
  }

  const { start, count } = getWindow();
  const rows = Math.min(count, state.totalSamples - start);
  const filter = parseAddressFilter();
  const html = [];

  for (let i = 0; i < rows; i++) {
    const sample = decodeSample(start + i);
    if (!filterMatches(sample, filter)) {
      continue;
    }

    html.push(`
      <tr data-index="${sample.index}" class="${sample.index === state.selectedSample ? "selected" : ""}">
        <td>${sample.index}</td>
        <td>0x${hex(sample.raw, 16)}</td>
        <td>${sample.frame}</td>
        <td>${sample.tick}</td>
        <td>${sample.line}</td>
        <td>${sample.cycle}</td>
        <td>0x${hex(sample.addr, 4)}</td>
        <td>0x${hex(sample.data, 2)}</td>
        <td>${sample.r_w ? "R" : "W"}</td>
        ${bitCell(sample.phi2)}
        ${bitCell(sample.ba)}
        ${bitCell(sample.irq)}
        ${bitCell(sample.dma)}
        ${bitCell(sample.charen)}
        ${bitCell(sample.hiram)}
        ${bitCell(sample.loram)}
        ${bitCell(sample.port01_chl_match)}
      </tr>
    `);
  }

  els.sampleRows.innerHTML = html.length
    ? html.join("")
    : `<tr><td colspan="17">No samples in the displayed table range match the filter.</td></tr>`;
}

function bitCell(value) {
  return `<td class="${value ? "bit-high" : "bit-low"}">${value}</td>`;
}

function updateSummary() {
  const { start, count } = getWindow();
  els.fileName.textContent = state.file ? state.file.name : "No file loaded";
  els.fileSize.textContent = state.file ? formatBytes(state.file.size) : "-";
  els.sampleTotal.textContent = state.file ? state.totalSamples.toLocaleString() : "-";
  els.windowInfo.textContent = state.file
    ? `${start.toLocaleString()} to ${(start + Math.max(0, count - 1)).toLocaleString()}`
    : "-";
}

async function render() {
  const token = ++state.renderToken;
  const { start, count } = getWindow();
  await ensureWindowLoaded(start, count);

  if (token !== state.renderToken) {
    return;
  }

  updateSummary();
  drawWaveforms();
  renderTable();
}

async function applyFilterAndRender() {
  const filter = parseAddressFilter();
  if (filter && state.file) {
    const start = Math.max(0, Number.parseInt(els.startSample.value || "0", 10));
    const match = await findMatchingSample(start, filter);
    if (match !== null) {
      els.startSample.value = match;
      state.selectedSample = match;
    }
  }

  await render();
}

async function loadFile(file) {
  state.file = file;
  state.buffer = null;
  state.view = null;
  state.sliceByteStart = 0;
  state.sliceByteEnd = 0;
  state.totalSamples = Math.floor(file.size / SAMPLE_BYTES);
  els.startSample.max = Math.max(0, state.totalSamples - 1);
  await render();
}

els.fileInput.addEventListener("change", (event) => {
  const file = event.target.files[0];
  if (file) {
    loadFile(file).catch((error) => {
      console.error(error);
      els.sampleRows.innerHTML = `<tr><td colspan="17">Failed to load file: ${error.message}</td></tr>`;
    });
  }
});

els.applyButton.addEventListener("click", applyFilterAndRender);
els.reverseLanes.addEventListener("change", applyFilterAndRender);
els.addressFilter.addEventListener("change", applyFilterAndRender);

els.prevButton.addEventListener("click", () => {
  const step = Math.max(16, Number.parseInt(els.sampleCount.value || "512", 10));
  els.startSample.value = Math.max(0, Number.parseInt(els.startSample.value || "0", 10) - step);
  render();
});

els.nextButton.addEventListener("click", () => {
  const step = Math.max(16, Number.parseInt(els.sampleCount.value || "512", 10));
  els.startSample.value = Math.min(
    Math.max(0, state.totalSamples - 1),
    Number.parseInt(els.startSample.value || "0", 10) + step
  );
  render();
});

els.sampleRows.addEventListener("click", (event) => {
  const row = event.target.closest("tr[data-index]");
  if (!row) {
    return;
  }

  state.selectedSample = Number.parseInt(row.dataset.index, 10);
  render();
});

window.addEventListener("resize", () => {
  if (state.view) {
    drawWaveforms();
  }
});

drawWaveforms();
