import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";

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

function parseInteger(name, fallback, min, max) {
  const value = Number.parseInt(argValue(name, String(fallback)), 10);
  if (!Number.isFinite(value)) return fallback;
  return Math.max(min, Math.min(max, value));
}

function parseNumber(name, fallback, min, max) {
  const value = Number.parseFloat(argValue(name, String(fallback)));
  if (!Number.isFinite(value)) return fallback;
  return Math.max(min, Math.min(max, value));
}

function padIndex(index) {
  return String(index).padStart(4, "0");
}

function readViewBox(svgText) {
  const match = svgText.match(/viewBox="([^"]+)"/i);
  if (!match) {
    throw new Error("Cannot find viewBox in component SVG");
  }

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

function extractSvgBody(svgText) {
  const match = svgText.match(/<svg\b[^>]*>([\s\S]*?)<\/svg>/i);
  if (!match) return "";
  return match[1]
    .replace(/<metadata\b[\s\S]*?<\/metadata>/gi, "")
    .replace(/<title\b[\s\S]*?<\/title>/gi, "")
    .trim();
}

function extractSourceMarkup(svgText) {
  const start = svgText.indexOf("<g ");
  const end = svgText.lastIndexOf("</g>");
  if (start >= 0 && end >= start) {
    return svgText.slice(start, end + 4);
  }

  const body = extractSvgBody(svgText);
  if (!body || !/<path\b/i.test(body)) {
    throw new Error("Cannot find drawable path markup in input SVG");
  }
  return body;
}

function componentIndex(name) {
  const match = name.match(/_(\d+)\.svg$/i);
  return match ? Number.parseInt(match[1], 10) : 0;
}

function componentFilesFor(image, channel) {
  const compDir = join("results_v2", image, "comp");
  if (!existsSync(compDir)) return [];

  const prefix = `${channel}_`;
  return readdirSync(compDir)
    .filter((name) => name.startsWith(prefix) && name.endsWith(".svg"))
    .sort((a, b) => componentIndex(a) - componentIndex(b))
    .map((name) => join(compDir, name));
}

function buildComponentOrderedMarkup(files) {
  return files.map((file, index) => {
    const source = readFileSync(file, "utf8");
    return `  <g class="ordered-component" data-index="${index}">
${extractSourceMarkup(source)}
  </g>`;
  }).join("\n");
}

function json(value) {
  return JSON.stringify(value);
}

function makeAnimatedSvg({
  image,
  channel,
  component,
  sourceLabel,
  sourceGroup,
  viewBox,
  samples,
  arms,
  duration,
  hold,
  dftMode,
  fixedCenter,
  penUpSamples
}) {
  const title = `${image} ${sourceLabel} DFT animation`;
  const center = {
    x: viewBox.x + viewBox.width * 0.5,
    y: viewBox.y + viewBox.height * 0.5
  };

  return `<?xml version="1.0" encoding="utf-8"?>
<svg xmlns="http://www.w3.org/2000/svg"
     width="${viewBox.width}" height="${viewBox.height}" viewBox="${viewBox.raw}">
  <title>${title}</title>
  <style>
    #background { fill: #fbfaf6; }
    #sourceLayer { opacity: 0.18; }
    #tracePath { fill: none; stroke: #087f8c; stroke-width: 2.2; stroke-linecap: round; stroke-linejoin: round; }
    #rebuildPath { fill: none; stroke: rgba(8, 127, 140, 0.22); stroke-width: 1.1; stroke-linecap: round; stroke-linejoin: round; }
    .arm-line { stroke: rgba(184, 107, 37, 0.76); stroke-width: 1; }
    .arm-circle { fill: none; stroke: rgba(184, 107, 37, 0.17); stroke-width: 1; }
    #armHead { fill: #b23a48; stroke: none; }
  </style>
  <rect id="background" x="${viewBox.x}" y="${viewBox.y}" width="${viewBox.width}" height="${viewBox.height}"/>
  <g id="sourceLayer">
${sourceGroup}
  </g>
  <path id="rebuildPath"/>
  <path id="tracePath"/>
  <g id="armLayer"></g>
  <circle id="armHead" r="3.5"/>
  <script><![CDATA[
(() => {
  const SAMPLE_COUNT = ${samples};
  const ARM_COUNT = ${arms};
  const DURATION_MS = ${duration * 1000};
  const HOLD_MS = ${hold * 1000};
  const CYCLE_MS = DURATION_MS + HOLD_MS;
  const DFT_MODE = ${json(dftMode)};
  const FIXED_CENTER = ${fixedCenter ? "true" : "false"};
  const PEN_UP_SAMPLES = ${penUpSamples};
  const CENTER_POINT = ${json(center)};
  const TAU = Math.PI * 2;
  const svg = document.documentElement;
  const sourceLayer = document.getElementById("sourceLayer");
  const armLayer = document.getElementById("armLayer");
  const rebuildPath = document.getElementById("rebuildPath");
  const tracePath = document.getElementById("tracePath");
  const armHead = document.getElementById("armHead");

  function transformPoint(point, matrix) {
    const svgPoint = svg.createSVGPoint();
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

  function samplePaths() {
    const paths = Array.from(sourceLayer.querySelectorAll("path"));
    const loops = [];
    let totalLength = 0;

    for (const path of paths) {
      for (const d of splitPathDataIntoLoops(path.getAttribute("d"))) {
        const loopPath = document.createElementNS("http://www.w3.org/2000/svg", "path");
        loopPath.setAttribute("d", d);
        const parent = path.parentNode || sourceLayer;
        parent.appendChild(loopPath);

        let length = 0;
        try {
          length = loopPath.getTotalLength();
        } catch (err) {
          length = 0;
        }
        if (length <= 0) {
          loopPath.remove();
          continue;
        }

        loopPath.setAttribute("opacity", "0");
        loopPath.setAttribute("fill", "none");
        const matrix = loopPath.getCTM();
        if (!matrix) {
          loopPath.remove();
          continue;
        }

        loops.push({ path: loopPath, length, matrix });
        totalLength += length;
      }
    }

    if (!loops.length || totalLength <= 0) return [];
    return loops;
  }

  function sampleLoop(loop, count) {
    const points = [];
    for (let i = 0; i < count; ++i) {
      const ratio = i / Math.max(1, count);
      let distance;
      if (Number.isFinite(loop.startOffset)) {
        distance = loop.startOffset + loop.length * (loop.reversed ? -ratio : ratio);
        distance = ((distance % loop.length) + loop.length) % loop.length;
      } else {
        distance = loop.length * (loop.reversed ? 1 - ratio : ratio);
      }
      points.push(transformPoint(loop.path.getPointAtLength(distance), loop.matrix));
    }
    return points;
  }

  function transformedPointAt(loop, distance) {
    const bounded = Math.max(0, Math.min(loop.length, distance));
    return transformPoint(loop.path.getPointAtLength(bounded), loop.matrix);
  }

  function isClosedLoop(loop) {
    const start = transformedPointAt(loop, 0);
    const end = transformedPointAt(loop, loop.length);
    return Math.hypot(start.x - end.x, start.y - end.y) <= Math.max(0.001, loop.length * 0.000001);
  }

  function loopEnd(loop) {
    if (Number.isFinite(loop.startOffset)) return transformedPointAt(loop, loop.startOffset);
    return transformedPointAt(loop, loop.reversed ? 0 : loop.length);
  }

  function nearestLoopChoice(current, loop) {
    if (isClosedLoop(loop)) {
      let best = { distance: Infinity, reversed: false, startOffset: 0 };
      const candidates = Math.max(16, Math.min(96, Math.ceil(loop.length / 8)));
      for (let i = 0; i < candidates; ++i) {
        const offset = loop.length * i / candidates;
        const point = transformedPointAt(loop, offset);
        const distance = Math.hypot(current.x - point.x, current.y - point.y);
        if (distance < best.distance) best = { distance, reversed: false, startOffset: offset };
      }
      return best;
    }

    const start = transformedPointAt(loop, 0);
    const end = transformedPointAt(loop, loop.length);
    const startDistance = Math.hypot(current.x - start.x, current.y - start.y);
    const endDistance = Math.hypot(current.x - end.x, current.y - end.y);
    return endDistance < startDistance
      ? { distance: endDistance, reversed: true }
      : { distance: startDistance, reversed: false };
  }

  function orderLoopsNearest(loops) {
    const sourceLoops = loops.filter((loop) => loop.length > 0);
    if (sourceLoops.length < 2) return sourceLoops;

    const ordered = [];
    const used = new Set();
    let cursor = null;
    while (ordered.length < sourceLoops.length) {
      let bestIndex = -1;
      let bestChoice = { distance: Infinity, reversed: false };
      if (!cursor && ordered.length === 0) {
        bestIndex = 0;
      } else {
        for (let i = 0; i < sourceLoops.length; ++i) {
          if (used.has(i)) continue;
          const choice = nearestLoopChoice(cursor, sourceLoops[i]);
          if (choice.distance < bestChoice.distance) {
            bestChoice = choice;
            bestIndex = i;
          }
        }
      }
      if (bestIndex < 0) break;
      used.add(bestIndex);
      const selected = { ...sourceLoops[bestIndex], reversed: bestChoice.reversed, startOffset: bestChoice.startOffset };
      ordered.push(selected);
      cursor = loopEnd(selected);
    }
    return ordered;
  }

  function allocateSampleCounts(loops, targetCount) {
    const sourceLoops = loops.filter((loop) => loop.length > 0);
    if (!sourceLoops.length) return [];

    const totalLength = sourceLoops.reduce((sum, loop) => sum + loop.length, 0);
    const minCount = sourceLoops.length <= targetCount ? 1 : 0;
    const counts = sourceLoops.map((loop) => {
      const exact = targetCount * loop.length / Math.max(1, totalLength);
      const base = Math.max(minCount, Math.floor(exact));
      return { loop, count: base, remainder: exact - Math.floor(exact) };
    });

    let countSum = counts.reduce((sum, entry) => sum + entry.count, 0);
    while (countSum > targetCount) {
      let best = -1;
      for (let i = 0; i < counts.length; ++i) {
        if (counts[i].count <= minCount) continue;
        if (best < 0 || counts[i].remainder < counts[best].remainder) best = i;
      }
      if (best < 0) break;
      counts[best].count -= 1;
      countSum -= 1;
    }

    while (countSum < targetCount) {
      let best = 0;
      for (let i = 1; i < counts.length; ++i) {
        if (counts[i].remainder > counts[best].remainder) best = i;
      }
      counts[best].count += 1;
      counts[best].remainder = 0;
      countSum += 1;
    }

    return counts;
  }

  function sampleSeparateLoops(loops) {
    const totalLength = loops.reduce((sum, loop) => sum + loop.length, 0);
    return loops.map((loop) => {
      const loopSamples = Math.max(64, Math.round(SAMPLE_COUNT * loop.length / Math.max(1, totalLength)));
      return { points: sampleLoop(loop, loopSamples), length: loop.length };
    }).filter((loop) => loop.points.length >= 2);
  }

  function stitchLoops(loops) {
    const sourceLoops = orderLoopsNearest(loops);
    if (!sourceLoops.length) return [];

    const targetCount = Math.max(2, SAMPLE_COUNT);
    const breakCount = Math.max(0, sourceLoops.length - 1);
    const maxTransitionTotal = Math.floor(targetCount * 0.25);
    const transitionCount = breakCount > 0
      ? Math.max(0, Math.min(PEN_UP_SAMPLES, Math.floor(maxTransitionTotal / breakCount)))
      : 0;
    const strokeTargetCount = Math.max(2, targetCount - transitionCount * breakCount);
    const totalLength = sourceLoops.reduce((sum, loop) => sum + loop.length, 0);
    const counts = allocateSampleCounts(sourceLoops, strokeTargetCount);
    const points = [];
    const breaks = [];
    for (const { loop, count } of counts) {
      if (count <= 0) continue;
      const loopPoints = sampleLoop(loop, count);
      if (loopPoints.length < 1) continue;
      if (points.length > 0) {
        const from = points[points.length - 1];
        const to = loopPoints[0];
        for (let i = 1; i <= transitionCount; ++i) {
          const t = i / (transitionCount + 1);
          breaks.push(points.length);
          points.push({
            x: from.x + (to.x - from.x) * t,
            y: from.y + (to.y - from.y) * t
          });
        }
        breaks.push(points.length);
      }
      points.push(...loopPoints);
    }

    return points.length >= 2 ? [{ points, length: totalLength, breaks }] : [];
  }

  function prepareLoops(loops) {
    let totalLength = 0;
    const dftLoops = DFT_MODE === "single" ? stitchLoops(loops) : sampleSeparateLoops(loops);
    const prepared = dftLoops.map((loop) => {
      const coeffs = orderedCoefficients(computeDft(loop.points)).slice(0, Math.min(ARM_COUNT + 1, loop.points.length));
      const trace = loop.points.map((_, index) => reconstructAt(index / loop.points.length, coeffs));
      totalLength += loop.length;
      return { ...loop, coeffs, trace, start: 0, end: 0 };
    });

    let cursor = 0;
    for (const loop of prepared) {
      loop.start = cursor / Math.max(1, totalLength);
      cursor += loop.length;
      loop.end = cursor / Math.max(1, totalLength);
    }

    return prepared;
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

  function epicycleAt(t, coeffs) {
    const points = [];
    const dc = coeffs.find((coef) => coef.freq === 0);
    let x = FIXED_CENTER ? CENTER_POINT.x : (dc ? dc.re : 0);
    let y = FIXED_CENTER ? CENTER_POINT.y : (dc ? dc.im : 0);
    points.push({ x, y, radius: 0 });

    for (const coef of coeffs) {
      if (coef.freq === 0 && !FIXED_CENTER) continue;
      const angle = TAU * coef.freq * t;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      const re = FIXED_CENTER && coef.freq === 0 ? coef.re - CENTER_POINT.x : coef.re;
      const im = FIXED_CENTER && coef.freq === 0 ? coef.im - CENTER_POINT.y : coef.im;
      const nextX = x + re * cos - im * sin;
      const nextY = y + re * sin + im * cos;
      points.push({ x: nextX, y: nextY, fromX: x, fromY: y, radius: Math.hypot(re, im) });
      x = nextX;
      y = nextY;
    }
    return points;
  }

  function pathData(points, limit, close, breaks = []) {
    if (points.length < 2) return "";
    const count = Math.max(2, Math.min(points.length, limit ?? points.length));
    const breakSet = new Set(breaks.filter((index) => index > 0 && index < count));
    let data = "M " + points[0].x.toFixed(2) + " " + points[0].y.toFixed(2);
    for (let i = 1; i < count; ++i) {
      if (breakSet.has(i)) {
        if (close) data += " Z";
        data += " M " + points[i].x.toFixed(2) + " " + points[i].y.toFixed(2);
      } else {
        data += " L " + points[i].x.toFixed(2) + " " + points[i].y.toFixed(2);
      }
    }
    return close && count >= points.length ? data + " Z" : data;
  }

  function createArmElements(loopCount, count) {
    const loopElements = [];
    for (let loopIndex = 0; loopIndex < loopCount; ++loopIndex) {
      const group = document.createElementNS("http://www.w3.org/2000/svg", "g");
      armLayer.appendChild(group);
      const arms = [];
      for (let i = 0; i < count; ++i) {
        const circle = document.createElementNS("http://www.w3.org/2000/svg", "circle");
        const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
        circle.setAttribute("class", "arm-circle");
        line.setAttribute("class", "arm-line");
        group.append(circle, line);
        arms.push({ circle, line });
      }
      loopElements.push(arms);
    }
    return loopElements;
  }

  function syncArms(chain, armElements) {
    for (let i = 1; i < chain.length; ++i) {
      const arm = chain[i];
      const el = armElements[i - 1];
      if (!el) continue;
      el.circle.setAttribute("cx", arm.fromX.toFixed(2));
      el.circle.setAttribute("cy", arm.fromY.toFixed(2));
      el.circle.setAttribute("r", Math.max(0, arm.radius).toFixed(2));
      el.line.setAttribute("x1", arm.fromX.toFixed(2));
      el.line.setAttribute("y1", arm.fromY.toFixed(2));
      el.line.setAttribute("x2", arm.x.toFixed(2));
      el.line.setAttribute("y2", arm.y.toFixed(2));
    }
  }

  const loops = prepareLoops(samplePaths());
  if (!loops.length) return;

  const maxArms = Math.max(...loops.map((loop) => loop.coeffs.length - 1));
  const armElements = createArmElements(loops.length, maxArms);
  rebuildPath.setAttribute("d", loops.map((loop) => pathData(loop.trace, loop.trace.length, true, loop.breaks)).join(" "));

  let start = null;
  function frame(now) {
    if (start === null) start = now;
    const cycleTime = (now - start) % CYCLE_MS;
    const t = Math.min(1, cycleTime / DURATION_MS);
    const drawnPaths = [];
    let activeEnd = null;

    for (let loopIndex = 0; loopIndex < loops.length; ++loopIndex) {
      const loop = loops[loopIndex];
      if (t >= loop.end) {
        drawnPaths.push(pathData(loop.trace, loop.trace.length, true, loop.breaks));
        continue;
      }
      if (t >= loop.start && t < loop.end) {
        const localT = (t - loop.start) / Math.max(0.0001, loop.end - loop.start);
        drawnPaths.push(pathData(loop.trace, Math.floor(localT * loop.trace.length) + 2, false, loop.breaks));
        const chain = epicycleAt(localT, loop.coeffs);
        syncArms(chain, armElements[loopIndex]);
        activeEnd = chain[chain.length - 1];
      }
    }

    if (!activeEnd) {
      const loopIndex = loops.length - 1;
      const chain = epicycleAt(1, loops[loopIndex].coeffs);
      syncArms(chain, armElements[loopIndex]);
      activeEnd = chain[chain.length - 1];
    }

    armHead.setAttribute("cx", activeEnd.x.toFixed(2));
    armHead.setAttribute("cy", activeEnd.y.toFixed(2));
    tracePath.setAttribute("d", drawnPaths.join(" "));
    requestAnimationFrame(frame);
  }

  requestAnimationFrame(frame);
})();
  ]]></script>
</svg>
`;
}

const image = argValue("image", "art");
const channel = argValue("channel", "XDoG_Guide");
const component = parseInteger("component", 0, 0, 9999);
const baseSamples = parseInteger("samples", 2048, 64, 65536);
const duration = parseNumber("duration", 8, 1, 120);
const hold = parseNumber("hold", 3, 0, 60);
const penUpSamples = parseInteger("pen-up-samples", 8, 0, 128);
const sourceMode = argValue("source", "component").toLowerCase();
const dftMode = argValue("dft-mode", sourceMode === "full" ? "single" : "loops").toLowerCase();
const fixedCenter = parseBoolean("fixed-center", false) || argValue("center", "source").toLowerCase() === "viewbox";
const fullSamplesPerComponent = parseBoolean("full-samples-per-component", false) ||
  argValue("full-samples", "total").toLowerCase() === "per-component";
const maxFullSamples = parseInteger("max-full-samples", 16384, 64, 262144);

if (sourceMode !== "component" && sourceMode !== "full") {
  throw new Error(`Unsupported source: ${sourceMode}. Use component or full.`);
}
if (dftMode !== "loops" && dftMode !== "single") {
  throw new Error(`Unsupported dft-mode: ${dftMode}. Use loops or single.`);
}

const input = argValue(
  "input",
  sourceMode === "full"
    ? join("results_v2", image, `${channel}.svg`)
    : join("results_v2", image, "comp", `${channel}_${padIndex(component)}.svg`)
);
const output = argValue(
  "output",
  sourceMode === "full"
    ? join("results_v2", image, "dft_anim", `${channel}_full_${dftMode}${fixedCenter ? "_center" : ""}_dft.svg`)
    : join("results_v2", image, "dft_anim", `${channel}_${padIndex(component)}${fixedCenter ? "_center" : ""}_dft.svg`)
);

if (!existsSync(input)) {
  throw new Error(`Input SVG not found: ${input}`);
}

const source = readFileSync(input, "utf8");
const viewBox = readViewBox(source);
const orderedComponentFiles = sourceMode === "full" ? componentFilesFor(image, channel) : [];
const sampleScale = sourceMode === "full" && fullSamplesPerComponent
  ? Math.max(1, orderedComponentFiles.length || 1)
  : 1;
const samples = sourceMode === "full" && fullSamplesPerComponent
  ? Math.min(baseSamples * sampleScale, maxFullSamples)
  : baseSamples;
const arms = parseInteger("arms", 256, 1, samples - 1);
const sourceGroup = orderedComponentFiles.length > 0
  ? buildComponentOrderedMarkup(orderedComponentFiles)
  : extractSourceMarkup(source);
const sourceLabel = sourceMode === "full" ? `${channel}_full` : `${channel}_${padIndex(component)}`;
const animatedSvg = makeAnimatedSvg({
  image,
  channel,
  component,
  sourceLabel,
  sourceGroup,
  viewBox,
  samples,
  arms,
  duration,
  hold,
  dftMode,
  fixedCenter,
  penUpSamples
});

mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, animatedSvg, "utf8");

console.log(`Wrote ${output}`);
console.log(`Input: ${input}`);
console.log(`Source: ${sourceMode}, DFT mode: ${dftMode}, fixed center: ${fixedCenter ? "yes" : "no"}`);
if (sourceMode === "full") {
  console.log(`Sampling order: ${orderedComponentFiles.length > 0 ? `${orderedComponentFiles.length} component SVGs` : "raw full SVG paths"}`);
  if (fullSamplesPerComponent) {
    console.log(`Full samples: ${baseSamples} * ${sampleScale} capped at ${maxFullSamples} -> ${samples}`);
  }
  console.log(`Pen-up transition samples per break: ${penUpSamples}`);
}
console.log(`Samples: ${samples}, arms: ${arms}, duration: ${duration}s, hold: ${hold}s`);
