import { createReadStream, existsSync, readdirSync, statSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, normalize, resolve } from "node:path";
import { spawn } from "node:child_process";

const root = process.cwd();
const requestedPort = Number(process.argv.find((arg) => /^\d+$/.test(arg)) || 5174);
const shouldOpen = process.argv.includes("--open");

const types = {
  ".bmp": "image/bmp",
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml; charset=utf-8",
  ".webm": "video/webm"
};

function send(response, status, text) {
  response.writeHead(status, { "content-type": "text/plain; charset=utf-8" });
  response.end(text);
}

function sendJson(response, value) {
  response.writeHead(200, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store"
  });
  response.end(JSON.stringify(value));
}

function safeName(value) {
  return /^[A-Za-z0-9_-]+$/.test(value || "") ? value : "";
}

const server = createServer((request, response) => {
  const url = new URL(request.url, "http://localhost");
  if (url.pathname === "/api/images") {
    const dir = normalize(resolve(join(root, "results_v2")));
    if (!dir.startsWith(root) || !existsSync(dir)) {
      sendJson(response, { images: [] });
      return;
    }

    const images = readdirSync(dir)
      .filter((name) => safeName(name))
      .filter((name) => {
        const imageDir = join(dir, name);
        return statSync(imageDir).isDirectory() && existsSync(join(imageDir, "comp"));
      })
      .sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: "base" }));
    sendJson(response, { images });
    return;
  }

  if (url.pathname === "/api/components") {
    const image = safeName(url.searchParams.get("image"));
    const channel = safeName(url.searchParams.get("channel"));
    const dir = normalize(resolve(join(root, "results_v2", image, "comp")));

    if (!image || !channel || !dir.startsWith(root) || !existsSync(dir)) {
      sendJson(response, { files: [] });
      return;
    }

    const prefix = `${channel}_`;
    const files = readdirSync(dir)
      .filter((name) => name.startsWith(prefix) && name.endsWith(".svg"))
      .sort();
    sendJson(response, { files });
    return;
  }

  const rawPath = decodeURIComponent(url.pathname);
  const relativePath = rawPath === "/" ? "dft_viewer.html" : rawPath.slice(1);
  const fullPath = normalize(resolve(join(root, relativePath)));

  if (!fullPath.startsWith(root) || !existsSync(fullPath) || !statSync(fullPath).isFile()) {
    send(response, 404, "Not found");
    return;
  }

  response.writeHead(200, {
    "content-type": types[extname(fullPath).toLowerCase()] || "application/octet-stream",
    "cache-control": "no-store"
  });
  createReadStream(fullPath).pipe(response);
});

function openBrowser(url) {
  if (!shouldOpen) return;
  if (process.platform === "win32") {
    spawn("cmd", ["/c", "start", "", url], { detached: true, stdio: "ignore" }).unref();
  } else if (process.platform === "darwin") {
    spawn("open", [url], { detached: true, stdio: "ignore" }).unref();
  } else {
    spawn("xdg-open", [url], { detached: true, stdio: "ignore" }).unref();
  }
}

function listen(port, attemptsLeft = 20) {
  server.once("error", (error) => {
    if (error.code === "EADDRINUSE" && attemptsLeft > 0) {
      console.log(`Port ${port} is busy, trying ${port + 1}...`);
      listen(port + 1, attemptsLeft - 1);
      return;
    }

    console.error(error);
    process.exit(1);
  });

  server.listen(port, "127.0.0.1", () => {
    const url = `http://127.0.0.1:${port}/dft_viewer.html`;
    console.log(`DFT viewer: ${url}`);
    console.log("Keep this window open while using the viewer.");
    openBrowser(url);
  });
}

listen(Number.isFinite(requestedPort) ? requestedPort : 5174);
