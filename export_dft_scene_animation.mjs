import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

function hasArg(name) {
  const prefix = `--${name}=`;
  return process.argv.some((arg) => arg.startsWith(prefix));
}

function argValue(name, fallback) {
  const prefix = `--${name}=`;
  const found = process.argv.find((arg) => arg.startsWith(prefix));
  return found ? found.slice(prefix.length) : fallback;
}

function parseBoolean(name, fallback) {
  const prefix = `--${name}=`;
  const exact = `--${name}`;
  const found = process.argv.find((arg) => arg === exact || arg.startsWith(prefix));
  if (!found) return fallback;
  if (found === exact) return true;

  const value = found.slice(prefix.length).trim().toLowerCase();
  if (["1", "true", "yes", "on", "viewbox", "center"].includes(value)) return true;
  if (["0", "false", "no", "off", "source"].includes(value)) return false;
  return fallback;
}

function parseIntegerValue(value, fallback, min, max) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.max(min, Math.min(max, parsed));
}

function parseNumberValue(value, fallback, min, max) {
  const parsed = Number.parseFloat(value);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.max(min, Math.min(max, parsed));
}

function parseInteger(name, fallback, min, max) {
  const value = parseIntegerValue(argValue(name, String(fallback)), fallback, min, max);
  if (!Number.isFinite(value)) return fallback;
  return value;
}

function parseNumber(name, fallback, min, max) {
  const value = parseNumberValue(argValue(name, String(fallback)), fallback, min, max);
  if (!Number.isFinite(value)) return fallback;
  return value;
}

function readConfig(path) {
  if (!existsSync(path)) return {};

  const config = {};
  const lines = readFileSync(path, "utf8").split(/\r?\n/);
  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("#")) continue;

    const equals = trimmed.indexOf("=");
    if (equals <= 0) continue;

    const key = trimmed.slice(0, equals).trim();
    const value = trimmed.slice(equals + 1).trim();
    if (key) config[key] = value;
  }
  return config;
}

function configValue(config, key, fallback) {
  return Object.prototype.hasOwnProperty.call(config, key) ? config[key] : fallback;
}

function modeValue(config, mode, key, fallback) {
  return configValue(config, `${mode}.${key}`, configValue(config, key, fallback));
}

function readViewBox(svgText) {
  const match = svgText.match(/viewBox="([^"]+)"/i);
  if (!match) throw new Error("Cannot find viewBox");

  const parts = match[1].trim().split(/[\s,]+/).map(Number);
  if (parts.length !== 4 || parts.some((value) => !Number.isFinite(value))) {
    throw new Error(`Invalid viewBox: ${match[1]}`);
  }

  return {
    raw: match[1],
    x: parts[0],
    y: parts[1],
    width: parts[2],
    height: parts[3]
  };
}

function extractPotraceGroup(svgText) {
  const start = svgText.indexOf("<g ");
  const end = svgText.lastIndexOf("</g>");
  if (start < 0 || end < start) {
    throw new Error("Cannot find potrace group");
  }
  return svgText.slice(start, end + 4);
}

function componentIndex(name) {
  const match = name.match(/_(\d+)\.svg$/i);
  return match ? Number.parseInt(match[1], 10) : 0;
}

function htmlEscape(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function buildSourceComponents(files, compDir) {
  return files.map((name, index) => {
    const source = readFileSync(join(compDir, name), "utf8");
    const group = extractPotraceGroup(source);
    return `    <g class="source-component" data-index="${index}" data-file="${name}">
${group}
    </g>`;
  }).join("\n");
}

function makeHtml({ image, channel, mode, viewBox, componentCount, sourceComponents, samples, arms, duration, hold, simArmParts, targetFps, drawStride, fixedCenter }) {
  const title = `${image} ${channel} ${mode} DFT scene`;
  const modeLabel = mode === "sequential" ? "Sequential" : "Simultaneous";
  const center = {
    x: viewBox.x + viewBox.width * 0.5,
    y: viewBox.y + viewBox.height * 0.5
  };

  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${htmlEscape(title)}</title>
  <style>
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      overflow: hidden;
      background: #f4f1ea;
      color: #1e2424;
      font: 13px/1.4 "Segoe UI", Arial, sans-serif;
      letter-spacing: 0;
    }
    #stage {
      width: 100vw;
      height: 100vh;
      display: block;
      background: #fbfaf6;
    }
    #hud {
      position: fixed;
      left: 14px;
      top: 14px;
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 8px 10px;
      border: 1px solid rgba(30, 36, 36, 0.14);
      border-radius: 6px;
      background: rgba(255, 253, 248, 0.88);
      backdrop-filter: blur(8px);
    }
    #status {
      color: #5d6660;
      min-width: 220px;
    }
    button {
      min-height: 28px;
      border: 1px solid #d5cec0;
      border-radius: 6px;
      background: #fff;
      color: #1e2424;
      cursor: pointer;
    }
    #sourceSvg {
      position: fixed;
      left: -100000px;
      top: 0;
      width: ${viewBox.width}px;
      height: ${viewBox.height}px;
      opacity: 0;
      pointer-events: none;
      overflow: hidden;
    }
  </style>
</head>
<body>
  <canvas id="stage"></canvas>
  <div id="hud">
    <button id="playButton">Pause</button>
    <button id="restartButton">Restart</button>
    <div id="status">Preparing ${modeLabel}</div>
  </div>
  <svg id="sourceSvg" xmlns="http://www.w3.org/2000/svg"
       width="${viewBox.width}" height="${viewBox.height}" viewBox="${viewBox.raw}">
${sourceComponents}
  </svg>

  <script>
(() => {
  const MODE = ${JSON.stringify(mode)};
  const SAMPLE_COUNT = ${samples};
  const ARM_COUNT = ${arms};
  const DURATION_MS = ${duration * 1000};
  const HOLD_MS = ${hold * 1000};
  const CYCLE_MS = DURATION_MS + HOLD_MS;
  const SIM_ARM_PARTS = ${simArmParts};
  const TARGET_FPS = ${targetFps};
  const FRAME_INTERVAL_MS = TARGET_FPS > 0 ? 1000 / TARGET_FPS : 0;
  const DRAW_STRIDE = Math.max(1, ${drawStride});
  const COMPONENT_COUNT = ${componentCount};
  const VIEW_BOX = ${JSON.stringify(viewBox)};
  const FIXED_CENTER = ${fixedCenter ? "true" : "false"};
  const CENTER_POINT = ${JSON.stringify(center)};
  const TAU = Math.PI * 2;

  const canvas = document.getElementById("stage");
  const ctx = canvas.getContext("2d");
  const sourceSvg = document.getElementById("sourceSvg");
  const statusEl = document.getElementById("status");
  const playButton = document.getElementById("playButton");
  const restartButton = document.getElementById("restartButton");

  const state = {
    ready: false,
    playing: true,
    startTime: 0,
    pauseTime: 0,
    components: [],
    totalLength: 1,
    referenceCanvas: null,
    finalCanvas: null,
    finalTransformKey: "",
    lastRenderTime: 0
  };

  function setStatus(text) {
    statusEl.textContent = text;
  }

  function resizeCanvas() {
    const ratio = Math.max(1, window.devicePixelRatio || 1);
    const width = Math.max(320, Math.floor(window.innerWidth * ratio));
    const height = Math.max(240, Math.floor(window.innerHeight * ratio));
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
  }

  function viewportTransform() {
    const ratio = Math.max(1, window.devicePixelRatio || 1);
    const cssWidth = canvas.width / ratio;
    const cssHeight = canvas.height / ratio;
    const padding = 28;
    const scale = Math.min(
      (cssWidth - padding * 2) / Math.max(1, VIEW_BOX.width),
      (cssHeight - padding * 2) / Math.max(1, VIEW_BOX.height)
    );
    return {
      ratio,
      scale,
      cssWidth,
      cssHeight,
      offsetX: (cssWidth - VIEW_BOX.width * scale) * 0.5,
      offsetY: (cssHeight - VIEW_BOX.height * scale) * 0.5
    };
  }

  function transformKey(transform) {
    return [
      transform.ratio,
      transform.scale.toFixed(6),
      transform.offsetX.toFixed(2),
      transform.offsetY.toFixed(2),
      transform.cssWidth,
      transform.cssHeight,
      canvas.width,
      canvas.height
    ].join("|");
  }

  function toScreen(point, transform) {
    return {
      x: transform.offsetX + (point.x - VIEW_BOX.x) * transform.scale,
      y: transform.offsetY + (point.y - VIEW_BOX.y) * transform.scale
    };
  }

  function transformPoint(point, matrix) {
    const svgPoint = sourceSvg.createSVGPoint();
    svgPoint.x = point.x;
    svgPoint.y = point.y;
    const transformed = svgPoint.matrixTransform(matrix);
    return { x: transformed.x, y: transformed.y };
  }

  function splitPathDataIntoLoops(d) {
    const tokens = String(d || "").match(/[AaCcHhLlMmQqSsTtVvZz]|[-+]?(?:\\d*\\.\\d+|\\d+\\.?)(?:[eE][-+]?\\d+)?/g) || [];
    const paramCounts = { A: 7, C: 6, H: 1, L: 2, M: 2, Q: 4, S: 4, T: 2, V: 1 };
    const loops = [];
    let chunk = [];
    let command = "";
    let index = 0;
    let current = { x: 0, y: 0 };
    let subpathStart = { x: 0, y: 0 };

    const isCommand = (token) => /^[A-Za-z]$/.test(token);
    const numberAt = () => Number(tokens[index++]);
    const format = (value) => {
      const rounded = Math.abs(value) < 1e-9 ? 0 : value;
      return Number(rounded.toFixed(6)).toString();
    };
    const push = (cmd, values = []) => {
      chunk.push(values.length ? cmd + " " + values.map(format).join(" ") : cmd);
    };
    const finish = () => {
      if (chunk.length > 0) {
        loops.push(chunk.join(" "));
        chunk = [];
      }
    };
    const updateEndpoint = (upper, relative, values) => {
      const relPoint = (x, y) => relative ? { x: current.x + x, y: current.y + y } : { x, y };
      if (upper === "L") current = relPoint(values[0], values[1]);
      else if (upper === "H") current = { x: relative ? current.x + values[0] : values[0], y: current.y };
      else if (upper === "V") current = { x: current.x, y: relative ? current.y + values[0] : values[0] };
      else if (upper === "C") current = relPoint(values[4], values[5]);
      else if (upper === "S" || upper === "Q") current = relPoint(values[2], values[3]);
      else if (upper === "T") current = relPoint(values[0], values[1]);
      else if (upper === "A") current = relPoint(values[5], values[6]);
    };

    while (index < tokens.length) {
      if (isCommand(tokens[index])) {
        command = tokens[index++];
      } else if (!command) {
        break;
      }

      const upper = command.toUpperCase();
      const relative = command !== upper;

      if (upper === "Z") {
        push("Z");
        current = { ...subpathStart };
        finish();
        command = "";
        continue;
      }

      if (upper === "M") {
        finish();
        if (index + 1 >= tokens.length) break;
        let x = numberAt();
        let y = numberAt();
        if (relative) {
          x += current.x;
          y += current.y;
        }
        current = { x, y };
        subpathStart = { x, y };
        push("M", [x, y]);

        while (index + 1 < tokens.length && !isCommand(tokens[index])) {
          const lx = numberAt();
          const ly = numberAt();
          if (relative) {
            push("l", [lx, ly]);
            current = { x: current.x + lx, y: current.y + ly };
          } else {
            push("L", [lx, ly]);
            current = { x: lx, y: ly };
          }
        }
        command = relative ? "l" : "L";
        continue;
      }

      const count = paramCounts[upper];
      if (!count) break;

      while (index < tokens.length && !isCommand(tokens[index])) {
        if (index + count > tokens.length) break;
        const values = [];
        for (let i = 0; i < count; ++i) {
          if (isCommand(tokens[index])) break;
          values.push(numberAt());
        }
        if (values.length !== count) break;
        push(command, values);
        updateEndpoint(upper, relative, values);
      }
    }

    finish();
    return loops;
  }

  function sampleLoop(loopPath, matrix, sampleCount) {
    let length = 0;
    try {
      length = loopPath.getTotalLength();
    } catch (err) {
      length = 0;
    }
    if (length <= 0) return null;

    const points = [];
    for (let i = 0; i < sampleCount; ++i) {
      const distance = length * i / sampleCount;
      points.push(transformPoint(loopPath.getPointAtLength(distance), matrix));
    }
    return { points, length };
  }

  function sampleComponent(group) {
    const paths = Array.from(group.querySelectorAll("path"));
    const entries = [];
    let totalLength = 0;

    for (const path of paths) {
      for (const d of splitPathDataIntoLoops(path.getAttribute("d"))) {
        const loopPath = document.createElementNS("http://www.w3.org/2000/svg", "path");
        loopPath.setAttribute("d", d);
        const parent = path.parentNode || group;
        parent.appendChild(loopPath);

        const matrix = loopPath.getCTM();
        if (!matrix) {
          loopPath.remove();
          continue;
        }
        entries.push({ path: loopPath, matrix });
      }
    }

    for (const entry of entries) {
      let length = 0;
      try {
        length = entry.path.getTotalLength();
      } catch (err) {
        length = 0;
      }
      entry.length = length;
      totalLength += Math.max(0, length);
    }

    if (!entries.length || totalLength <= 0) return [];

    const loops = [];
    for (const entry of entries) {
      if (entry.length <= 0) continue;
      const loopSamples = Math.max(64, Math.round(SAMPLE_COUNT * entry.length / totalLength));
      const sampled = sampleLoop(entry.path, entry.matrix, loopSamples);
      if (sampled) loops.push(sampled);
    }

    return loops;
  }

  function computeDft(points) {
    const n = points.length;
    const half = Math.floor(n / 2);
    const coeffs = [];

    for (let k = 0; k < n; ++k) {
      const freq = k <= half ? k : k - n;
      let re = 0;
      let im = 0;

      for (let i = 0; i < n; ++i) {
        const angle = -TAU * freq * i / n;
        const cos = Math.cos(angle);
        const sin = Math.sin(angle);
        const x = points[i].x;
        const y = points[i].y;
        re += x * cos - y * sin;
        im += x * sin + y * cos;
      }

      re /= n;
      im /= n;
      coeffs.push({ freq, re, im, amp: Math.hypot(re, im) });
    }

    return coeffs;
  }

  function orderedCoefficients(coeffs) {
    const dc = coeffs.find((coef) => coef.freq === 0);
    const rest = coeffs.filter((coef) => coef.freq !== 0).sort((a, b) => b.amp - a.amp);
    return dc ? [dc, ...rest] : rest;
  }

  function reconstructAt(t, coeffs) {
    let x = FIXED_CENTER ? CENTER_POINT.x : 0;
    let y = FIXED_CENTER ? CENTER_POINT.y : 0;
    for (const coef of coeffs) {
      const angle = TAU * coef.freq * t;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      const re = FIXED_CENTER && coef.freq === 0 ? coef.re - CENTER_POINT.x : coef.re;
      const im = FIXED_CENTER && coef.freq === 0 ? coef.im - CENTER_POINT.y : coef.im;
      x += re * cos - im * sin;
      y += re * sin + im * cos;
    }
    return { x, y };
  }

  function buildTrace(points, coeffs) {
    return points.map((_, index) => reconstructAt(index / points.length, coeffs));
  }

  function epicycleAt(t, coeffs) {
    const chain = [];
    const dc = coeffs.find((coef) => coef.freq === 0);
    let x = FIXED_CENTER ? CENTER_POINT.x : (dc ? dc.re : 0);
    let y = FIXED_CENTER ? CENTER_POINT.y : (dc ? dc.im : 0);
    chain.push({ x, y, radius: 0 });

    for (const coef of coeffs) {
      if (coef.freq === 0 && !FIXED_CENTER) continue;
      const angle = TAU * coef.freq * t;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      const re = FIXED_CENTER && coef.freq === 0 ? coef.re - CENTER_POINT.x : coef.re;
      const im = FIXED_CENTER && coef.freq === 0 ? coef.im - CENTER_POINT.y : coef.im;
      const nextX = x + re * cos - im * sin;
      const nextY = y + re * sin + im * cos;
      chain.push({ x: nextX, y: nextY, fromX: x, fromY: y, radius: Math.hypot(re, im) });
      x = nextX;
      y = nextY;
    }
    return chain;
  }

  function drawPolyline(points, transform, color, width, closePath, progress = 1, stride = 1) {
    if (points.length < 2 || progress <= 0) return;
    const count = Math.max(2, Math.min(points.length, Math.floor(points.length * progress)));
    const first = toScreen(points[0], transform);

    ctx.beginPath();
    ctx.moveTo(first.x, first.y);
    for (let i = 1; i < count; i += stride) {
      const p = toScreen(points[i], transform);
      ctx.lineTo(p.x, p.y);
    }
    if (count > 2 && ((count - 1) % stride) !== 0) {
      const last = toScreen(points[count - 1], transform);
      ctx.lineTo(last.x, last.y);
    }
    if (closePath && count >= points.length) ctx.closePath();
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();
  }

  function drawArms(component, t, transform, alpha) {
    const chain = epicycleAt(t, component.coeffs);
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.lineWidth = 1;

    for (let i = 1; i < chain.length; ++i) {
      const arm = chain[i];
      const center = toScreen({ x: arm.fromX, y: arm.fromY }, transform);
      const tip = toScreen({ x: arm.x, y: arm.y }, transform);
      const radius = Math.abs(arm.radius * transform.scale);

      ctx.beginPath();
      ctx.arc(center.x, center.y, radius, 0, TAU);
      ctx.strokeStyle = "rgba(184, 107, 37, 0.18)";
      ctx.stroke();

      ctx.beginPath();
      ctx.moveTo(center.x, center.y);
      ctx.lineTo(tip.x, tip.y);
      ctx.strokeStyle = "rgba(184, 107, 37, 0.78)";
      ctx.stroke();
    }

    const end = chain[chain.length - 1];
    const head = toScreen({ x: end.x, y: end.y }, transform);
    ctx.beginPath();
    ctx.arc(head.x, head.y, 2.8, 0, TAU);
    ctx.fillStyle = "#b23a48";
    ctx.fill();
    ctx.restore();
  }

  function createLayerCanvas() {
    const layer = document.createElement("canvas");
    layer.width = canvas.width;
    layer.height = canvas.height;
    return layer;
  }

  function drawPolylineOn(context, points, transform, color, width, closePath, progress = 1, stride = 1) {
    if (points.length < 2 || progress <= 0) return;
    const count = Math.max(2, Math.min(points.length, Math.floor(points.length * progress)));
    const first = toScreen(points[0], transform);

    context.beginPath();
    context.moveTo(first.x, first.y);
    for (let i = 1; i < count; i += stride) {
      const p = toScreen(points[i], transform);
      context.lineTo(p.x, p.y);
    }
    if (count > 2 && ((count - 1) % stride) !== 0) {
      const last = toScreen(points[count - 1], transform);
      context.lineTo(last.x, last.y);
    }
    if (closePath && count >= points.length) context.closePath();
    context.strokeStyle = color;
    context.lineWidth = width;
    context.lineJoin = "round";
    context.lineCap = "round";
    context.stroke();
  }

  function buildReferenceCanvas(transform) {
    const layer = createLayerCanvas();
    const context = layer.getContext("2d");
    context.setTransform(transform.ratio, 0, 0, transform.ratio, 0, 0);
    for (const component of state.components) {
      drawPolylineOn(context, component.points, transform, "rgba(30, 36, 36, 0.16)", 0.85, true, 1, DRAW_STRIDE);
    }
    return layer;
  }

  function buildFinalCanvas(transform) {
    const layer = createLayerCanvas();
    const context = layer.getContext("2d");
    context.setTransform(transform.ratio, 0, 0, transform.ratio, 0, 0);
    context.fillStyle = "#fbfaf6";
    context.fillRect(0, 0, transform.cssWidth, transform.cssHeight);
    if (state.referenceCanvas) {
      context.setTransform(1, 0, 0, 1, 0, 0);
      context.drawImage(state.referenceCanvas, 0, 0);
      context.setTransform(transform.ratio, 0, 0, transform.ratio, 0, 0);
    }
    for (const component of state.components) {
      drawPolylineOn(context, component.trace, transform, "rgba(8, 127, 140, 0.95)", 1.8, true, 1, DRAW_STRIDE);
    }
    return layer;
  }

  function prepare() {
    const groups = Array.from(sourceSvg.querySelectorAll(".source-component"));
    const components = [];
    let totalLength = 0;

    groups.forEach((group, index) => {
      setStatus(\`Preparing \${index + 1}/\${COMPONENT_COUNT}\`);
      const loops = sampleComponent(group);
      if (!loops.length) return;

      loops.forEach((sampled, loopIndex) => {
        const coeffs = orderedCoefficients(computeDft(sampled.points))
          .slice(0, Math.min(ARM_COUNT + 1, sampled.points.length));
        const trace = buildTrace(sampled.points, coeffs);
        const component = {
          file: group.dataset.file,
          loopIndex,
          points: sampled.points,
          coeffs,
          trace,
          length: sampled.length,
          start: 0,
          end: 0
        };
        totalLength += sampled.length;
        components.push(component);
      });
    });

    const groupsByFile = [];
    const groupMap = new Map();
    for (const component of components) {
      if (!groupMap.has(component.file)) {
        const group = { file: component.file, components: [], length: 0 };
        groupMap.set(component.file, group);
        groupsByFile.push(group);
      }
      const group = groupMap.get(component.file);
      group.components.push(component);
      group.length += component.length;
    }

    const groupSpan = 1 / Math.max(1, groupsByFile.length);
    groupsByFile.forEach((group, groupIndex) => {
      let cursor = groupIndex * groupSpan;
      const groupEnd = (groupIndex + 1) * groupSpan;
      for (const component of group.components) {
        const localSpan = groupSpan * (component.length / Math.max(1, group.length));
        component.start = cursor;
        cursor += localSpan;
        component.end = Math.min(groupEnd, cursor);
      }
      if (group.components.length > 0) {
        group.components[group.components.length - 1].end = groupEnd;
      }
    });

    for (const component of components) {
      if (!Number.isFinite(component.start) || !Number.isFinite(component.end) || component.end <= component.start) {
        component.start = 0;
        component.end = 1;
      }
    }

    const armVisible = new Set(
      [...components]
        .sort((a, b) => b.length - a.length)
        .slice(0, Math.max(0, SIM_ARM_PARTS))
    );
    for (const component of components) {
      component.showArms = armVisible.has(component);
    }

    state.components = components;
    state.totalLength = Math.max(1, totalLength);
    state.ready = true;
    state.startTime = performance.now();
    setStatus(\`${modeLabel} | \${components.length} parts | \${SAMPLE_COUNT}/\${ARM_COUNT} | arms \${Math.min(SIM_ARM_PARTS, components.length)}\`);
  }

  function renderSequential(t, transform) {
    let active = state.components[0];
    let activeProgress = 0;

    for (const component of state.components) {
      if (t >= component.end) {
        drawPolyline(component.trace, transform, "rgba(8, 127, 140, 0.95)", 1.8, true, 1, DRAW_STRIDE);
      } else if (t >= component.start && t < component.end) {
        active = component;
        activeProgress = (t - component.start) / Math.max(0.0001, component.end - component.start);
        drawPolyline(component.trace, transform, "rgba(8, 127, 140, 0.98)", 2.1, false, activeProgress, DRAW_STRIDE);
      }
    }

    if (active) {
      drawArms(active, activeProgress, transform, 0.92);
    }
  }

  function renderSimultaneous(t, transform) {
    for (const component of state.components) {
      drawPolyline(component.trace, transform, "rgba(8, 127, 140, 0.82)", 1.55, false, t, DRAW_STRIDE);
    }

    if (t >= 1) return;

    for (const component of state.components) {
      if (!component.showArms) continue;
      drawArms(component, t, transform, 0.34);
    }
  }

  function render(now) {
    if (FRAME_INTERVAL_MS > 0 && now - state.lastRenderTime < FRAME_INTERVAL_MS) {
      requestAnimationFrame(render);
      return;
    }
    state.lastRenderTime = now;

    resizeCanvas();
    const transform = viewportTransform();
    const key = transformKey(transform);

    if (key !== state.finalTransformKey) {
      state.referenceCanvas = null;
      state.finalCanvas = null;
      state.finalTransformKey = key;
    }

    ctx.setTransform(transform.ratio, 0, 0, transform.ratio, 0, 0);
    ctx.clearRect(0, 0, transform.cssWidth, transform.cssHeight);
    ctx.fillStyle = "#fbfaf6";
    ctx.fillRect(0, 0, transform.cssWidth, transform.cssHeight);

    if (!state.ready) {
      requestAnimationFrame(render);
      return;
    }

    const elapsed = state.playing ? now - state.startTime : state.pauseTime - state.startTime;
    const cycleTime = ((elapsed % CYCLE_MS) + CYCLE_MS) % CYCLE_MS;
    const t = Math.min(1, cycleTime / DURATION_MS);

    if (!state.referenceCanvas) {
      state.referenceCanvas = buildReferenceCanvas(transform);
    }
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.drawImage(state.referenceCanvas, 0, 0);
    ctx.setTransform(transform.ratio, 0, 0, transform.ratio, 0, 0);

    if (t >= 1) {
      if (!state.finalCanvas) {
        state.finalCanvas = buildFinalCanvas(transform);
      }
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.drawImage(state.finalCanvas, 0, 0);
      requestAnimationFrame(render);
      return;
    }

    if (MODE === "sequential") {
      renderSequential(t, transform);
    } else {
      renderSimultaneous(t, transform);
    }

    requestAnimationFrame(render);
  }

  playButton.addEventListener("click", () => {
    state.playing = !state.playing;
    if (state.playing) {
      const pausedFor = performance.now() - state.pauseTime;
      state.startTime += pausedFor;
      playButton.textContent = "Pause";
    } else {
      state.pauseTime = performance.now();
      playButton.textContent = "Play";
    }
  });

  restartButton.addEventListener("click", () => {
    state.startTime = performance.now();
    state.pauseTime = state.startTime;
  });

  window.addEventListener("resize", resizeCanvas);
  resizeCanvas();
  setTimeout(prepare, 30);
  requestAnimationFrame(render);
})();
  </script>
</body>
</html>
`;
}

const image = argValue("image", "art");
const channel = argValue("channel", "XDoG_Guide");
const configPath = argValue("config", "dft_scene_params.txt");
const config = readConfig(configPath);
const mode = argValue("mode", configValue(config, "mode", "both"));
const outputDir = argValue("output-dir", join("results_v2", image, "dft_scene"));
const compDir = join("results_v2", image, "comp");
const fixedCenter = parseBoolean("fixed-center", false) || argValue("center", "source").toLowerCase() === "viewbox";

if (!existsSync(compDir)) {
  throw new Error(`Component folder not found: ${compDir}`);
}

const prefix = `${channel}_`;
const files = readdirSync(compDir)
  .filter((name) => name.startsWith(prefix) && name.endsWith(".svg"))
  .sort((a, b) => componentIndex(a) - componentIndex(b));

if (files.length === 0) {
  throw new Error(`No component SVGs found for ${image}/${channel}`);
}

const firstSource = readFileSync(join(compDir, files[0]), "utf8");
const viewBox = readViewBox(firstSource);
const sourceComponents = buildSourceComponents(files, compDir);
const modes = mode === "both" ? ["sequential", "simultaneous"] : [mode];

mkdirSync(outputDir, { recursive: true });

for (const sceneMode of modes) {
  if (sceneMode !== "sequential" && sceneMode !== "simultaneous") {
    throw new Error(`Unsupported mode: ${sceneMode}`);
  }

  const baseSamples = parseIntegerValue(modeValue(config, sceneMode, "samples", "512"), 512, 64, 4096);
  const samples = hasArg("samples") ? parseInteger("samples", baseSamples, 64, 4096) : baseSamples;
  const baseArms = parseIntegerValue(modeValue(config, sceneMode, "arms", "96"), 96, 1, samples - 1);
  const arms = hasArg("arms") ? parseInteger("arms", baseArms, 1, samples - 1) : Math.min(baseArms, samples - 1);
  const duration = hasArg("duration")
    ? parseNumber("duration", parseNumberValue(modeValue(config, sceneMode, "duration", "60"), 60, 3, 600), 3, 600)
    : parseNumberValue(modeValue(config, sceneMode, "duration", "60"), 60, 3, 600);
  const hold = hasArg("hold")
    ? parseNumber("hold", parseNumberValue(modeValue(config, sceneMode, "hold", "3"), 3, 0, 120), 0, 120)
    : parseNumberValue(modeValue(config, sceneMode, "hold", "3"), 3, 0, 120);
  const simArmParts = hasArg("sim-arm-parts")
    ? parseInteger("sim-arm-parts", parseIntegerValue(modeValue(config, sceneMode, "sim_arm_parts", "24"), 24, 0, 9999), 0, 9999)
    : parseIntegerValue(modeValue(config, sceneMode, "sim_arm_parts", "24"), 24, 0, 9999);
  const targetFps = hasArg("target-fps")
    ? parseInteger("target-fps", parseIntegerValue(modeValue(config, sceneMode, "target_fps", "30"), 30, 0, 120), 0, 120)
    : parseIntegerValue(modeValue(config, sceneMode, "target_fps", "30"), 30, 0, 120);
  const drawStride = hasArg("draw-stride")
    ? parseInteger("draw-stride", parseIntegerValue(modeValue(config, sceneMode, "draw_stride", "2"), 2, 1, 16), 1, 16)
    : parseIntegerValue(modeValue(config, sceneMode, "draw_stride", "2"), 2, 1, 16);

  const html = makeHtml({
    image,
    channel,
    mode: sceneMode,
    viewBox,
    componentCount: files.length,
    sourceComponents,
    samples,
    arms,
    duration,
    hold,
    simArmParts,
    targetFps,
    drawStride,
    fixedCenter
  });
  const output = join(outputDir, `${channel}_all_${sceneMode}${fixedCenter ? "_center" : ""}.html`);
  writeFileSync(output, html, "utf8");
  console.log(`Wrote ${output}`);
  console.log(`  ${sceneMode}: samples=${samples}, arms=${arms}, duration=${duration}s, hold=${hold}s, simultaneous arm parts=${simArmParts}, target fps=${targetFps}, draw stride=${drawStride}, fixed center=${fixedCenter ? "yes" : "no"}`);
}

console.log(`Components: ${files.length}`);
console.log(`Config: ${configPath}`);
