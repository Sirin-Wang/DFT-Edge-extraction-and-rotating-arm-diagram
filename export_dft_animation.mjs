import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";

function argValue(name, fallback) {
  const prefix = `--${name}=`;
  const found = process.argv.find((arg) => arg.startsWith(prefix));
  return found ? found.slice(prefix.length) : fallback;
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

function extractPotraceGroup(svgText) {
  const start = svgText.indexOf("<g ");
  const end = svgText.lastIndexOf("</g>");
  if (start < 0 || end < start) {
    throw new Error("Cannot find potrace path group in component SVG");
  }
  return svgText.slice(start, end + 4);
}

function json(value) {
  return JSON.stringify(value);
}

function makeAnimatedSvg({ image, channel, component, sourceGroup, viewBox, samples, arms, duration, hold }) {
  const title = `${image} ${channel}_${padIndex(component)} DFT animation`;

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

    return loops.map((loop) => {
      const loopSamples = Math.max(64, Math.round(SAMPLE_COUNT * loop.length / totalLength));
      const points = [];
      for (let i = 0; i < loopSamples; ++i) {
        const distance = loop.length * i / loopSamples;
        points.push(transformPoint(loop.path.getPointAtLength(distance), loop.matrix));
      }
      return { points, length: loop.length };
    }).filter((loop) => loop.points.length >= 2);
  }

  function prepareLoops(loops) {
    let totalLength = 0;
    const prepared = loops.map((loop) => {
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
    let x = 0;
    let y = 0;
    for (const coef of coeffs) {
      const angle = TAU * coef.freq * t;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      x += coef.re * cos - coef.im * sin;
      y += coef.re * sin + coef.im * cos;
    }
    return { x, y };
  }

  function epicycleAt(t, coeffs) {
    const points = [];
    const dc = coeffs.find((coef) => coef.freq === 0);
    let x = dc ? dc.re : 0;
    let y = dc ? dc.im : 0;
    points.push({ x, y, radius: 0 });

    for (const coef of coeffs) {
      if (coef.freq === 0) continue;
      const angle = TAU * coef.freq * t;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      const nextX = x + coef.re * cos - coef.im * sin;
      const nextY = y + coef.re * sin + coef.im * cos;
      points.push({ x: nextX, y: nextY, fromX: x, fromY: y, radius: coef.amp });
      x = nextX;
      y = nextY;
    }
    return points;
  }

  function pathData(points, limit, close) {
    if (points.length < 2) return "";
    const count = Math.max(2, Math.min(points.length, limit ?? points.length));
    let data = "M " + points[0].x.toFixed(2) + " " + points[0].y.toFixed(2);
    for (let i = 1; i < count; ++i) {
      data += " L " + points[i].x.toFixed(2) + " " + points[i].y.toFixed(2);
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
  rebuildPath.setAttribute("d", loops.map((loop) => pathData(loop.trace, loop.trace.length, true)).join(" "));

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
        drawnPaths.push(pathData(loop.trace, loop.trace.length, true));
        continue;
      }
      if (t >= loop.start && t < loop.end) {
        const localT = (t - loop.start) / Math.max(0.0001, loop.end - loop.start);
        drawnPaths.push(pathData(loop.trace, Math.floor(localT * loop.trace.length) + 2, false));
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
const samples = parseInteger("samples", 2048, 64, 8192);
const arms = parseInteger("arms", 256, 1, samples - 1);
const duration = parseNumber("duration", 8, 1, 120);
const hold = parseNumber("hold", 3, 0, 60);

const input = argValue(
  "input",
  join("results_v2", image, "comp", `${channel}_${padIndex(component)}.svg`)
);
const output = argValue(
  "output",
  join("results_v2", image, "dft_anim", `${channel}_${padIndex(component)}_dft.svg`)
);

if (!existsSync(input)) {
  throw new Error(`Input SVG not found: ${input}`);
}

const source = readFileSync(input, "utf8");
const viewBox = readViewBox(source);
const sourceGroup = extractPotraceGroup(source);
const animatedSvg = makeAnimatedSvg({ image, channel, component, sourceGroup, viewBox, samples, arms, duration, hold });

mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, animatedSvg, "utf8");

console.log(`Wrote ${output}`);
console.log(`Input: ${input}`);
console.log(`Samples: ${samples}, arms: ${arms}, duration: ${duration}s, hold: ${hold}s`);
