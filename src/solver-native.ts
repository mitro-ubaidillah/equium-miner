/**
 * Native C Equihash Solver — Persistent Worker Bridge
 *
 * Uses a long-running C worker process that reads commands from stdin
 * and streams results to stdout. This eliminates process-spawn overhead
 * (each spawn takes 50-200ms) and allows continuous mining.
 *
 * Architecture:
 *   Node.js ─ stdin (JSON cmd) ─→ C worker (pthreads) ─ stdout (JSON result) ─→ Node.js
 *
 * Worker is started once on initSolver() and reused across all mining cycles.
 */

import { createRequire } from "module";
import { existsSync } from "fs";
import { join, dirname } from "path";
import { fileURLToPath } from "url";
import { execSync, spawn, ChildProcess } from "child_process";
import { randomBytes } from "crypto";
import { hashUnderTarget, solutionHash } from "./equihash.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const PROJECT_ROOT = join(__dirname, "..");
const NATIVE_DIR = join(PROJECT_ROOT, "native");
const WORKER_PATH = join(NATIVE_DIR, "eqm_worker");

const COMPRESSED_SIZE = 68;
const I_BLOCK_LEN = 81;

// ─── Persistent worker process ───
let workerProc: ChildProcess | null = null;
let workerReady = false;
let useNative = false;

interface PendingRequest {
  resolve: (result: SolveResult | null) => void;
  reject: (err: Error) => void;
  startTime: number;
}

let pendingRequest: PendingRequest | null = null;
let stdoutBuffer = "";

interface SolveResult {
  nonce: Buffer;
  solution: Buffer;
  attempts: number;
  elapsed?: number;
  hps?: number;
}

/**
 * Build the native worker binary if missing.
 */
function ensureNativeBuilt(): boolean {
  if (existsSync(WORKER_PATH)) return true;

  console.log("  ⚙ Building native worker...");
  try {
    // Build worker directly (avoid shared-lib complexity)
    execSync(
      `gcc -O3 -march=native -DWORKER_MODE -o eqm_worker eqm_worker.c -lpthread`,
      { cwd: NATIVE_DIR, stdio: "pipe" }
    );
    console.log("  ✓ Native worker built");
    return true;
  } catch (e: any) {
    console.log(`  ✗ Build failed: ${e.message}`);
    return false;
  }
}

/**
 * Start persistent worker process (once)
 */
function startWorker(): boolean {
  if (workerProc && !workerProc.killed) return true;

  if (!ensureNativeBuilt()) return false;

  workerProc = spawn(WORKER_PATH, [], {
    cwd: NATIVE_DIR,
    stdio: ["pipe", "pipe", "pipe"],
  });

  workerProc.on("error", (err) => {
    console.log(`  ✗ Worker error: ${err.message}`);
    workerReady = false;
    workerProc = null;
  });

  workerProc.on("exit", (code) => {
    console.log(`  ⚠ Worker exited (code ${code})`);
    workerReady = false;
    workerProc = null;
    // Fail any pending request
    if (pendingRequest) {
      pendingRequest.reject(new Error("Worker exited unexpectedly"));
      pendingRequest = null;
    }
  });

  workerProc.stdout?.on("data", (data: Buffer) => {
    stdoutBuffer += data.toString();
    processStdoutBuffer();
  });

  workerProc.stderr?.on("data", (data: Buffer) => {
    const msg = data.toString().trim();
    if (msg.includes("ready")) {
      workerReady = true;
    }
    // Suppress other stderr unless it's an error
    if (msg && !msg.includes("[worker]")) {
      console.log(`  [worker stderr] ${msg}`);
    }
  });

  return true;
}

/**
 * Process lines from stdout buffer
 */
function processStdoutBuffer() {
  const lines = stdoutBuffer.split("\n");
  stdoutBuffer = lines.pop() || ""; // keep incomplete last line

  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) continue;

    try {
      const msg = JSON.parse(trimmed);

      if (msg.type === "result" || msg.found !== undefined) {
        if (pendingRequest) {
          const req = pendingRequest;
          pendingRequest = null;

          if (msg.found) {
            req.resolve({
              nonce: Buffer.from(msg.nonce, "hex"),
              solution: Buffer.from(msg.solution, "hex"),
              attempts: msg.attempts,
              elapsed: msg.elapsed,
              hps: msg.hps,
            });
          } else {
            req.resolve(null);
          }
        }
      } else if (msg.type === "error") {
        if (pendingRequest) {
          pendingRequest.reject(new Error(msg.message));
          pendingRequest = null;
        }
      }
    } catch {
      // Ignore non-JSON lines
    }
  }
}

/**
 * Send a solve command to the worker and wait for result.
 */
function solveViaWorker(
  input: Buffer,
  target: Buffer,
  numThreads: number,
  maxNonces: number,
  timeoutMs: number = 120_000
): Promise<SolveResult | null> {
  return new Promise((resolve, reject) => {
    if (!workerProc || !workerProc.stdin) {
      reject(new Error("Worker not started"));
      return;
    }

    if (pendingRequest) {
      reject(new Error("Worker busy (previous request pending)"));
      return;
    }

    pendingRequest = { resolve, reject, startTime: Date.now() };

    const cmd = JSON.stringify({
      cmd: "solve",
      input: input.toString("hex"),
      target: target.toString("hex"),
      threads: numThreads,
      max_nonces: maxNonces,
    });

    workerProc.stdin.write(cmd + "\n", (err) => {
      if (err) {
        if (pendingRequest === arguments.callee.caller as any) {
          pendingRequest = null;
        }
        reject(err);
      }
    });

    // Timeout
    const timer = setTimeout(() => {
      if (pendingRequest) {
        pendingRequest = null;
        resolve(null); // treat as no-solution instead of error
      }
    }, timeoutMs);

    // Clear timer on success/error
    const origResolve = resolve;
    resolve = (val) => { clearTimeout(timer); origResolve(val); };
  });
}

// ─── Public API ───

let initialized = false;
let threadCount = 4;

export function initSolver(): { native: boolean; threads: number } {
  if (initialized) return { native: useNative, threads: threadCount };

  const numCPUs = require("os").cpus().length;
  threadCount = Math.max(1, numCPUs - 1);

  if (startWorker()) {
    useNative = true;
    console.log("  ✓ Native C worker (persistent mode) started");
  } else {
    useNative = false;
    console.log("  ⚠ Using JS fallback solver (slower)");
  }

  initialized = true;
  return { native: useNative, threads: threadCount };
}

/**
 * Shutdown worker cleanly
 */
export function shutdownSolver() {
  if (workerProc && workerProc.stdin) {
    workerProc.stdin.write('{"cmd":"stop"}\n');
    setTimeout(() => {
      if (workerProc && !workerProc.killed) workerProc.kill("SIGTERM");
    }, 500);
  }
}

// ─── JS fallback (unchanged from before) ───
const require = createRequire(import.meta.url);
const blake2b = require("blake2b");

const N = 96, K = 5;
const COLLISION_BITS = 16;
const COLLISION_BYTES = 2;
const HASH_OUTPUT_LEN = 60;
const INDICES_PER_HASH = 5;
const N_BYTES = 12;
const NUM_INITIAL = 1 << 17;
const SOLUTION_COUNT = 32;
const BITS_PER_INDEX = 17;
const MAX_BUCKET = 24;

function generateLeafJS(input: Buffer, nonce: Buffer, index: number): Buffer {
  const personal = Buffer.alloc(16);
  personal.write("ZcashPoW", 0, 8, "ascii");
  personal.writeUInt32LE(N, 8);
  personal.writeUInt32LE(K, 12);

  const indexGroup = Math.floor(index / INDICES_PER_HASH);
  const indexBuf = Buffer.alloc(4);
  indexBuf.writeUInt32LE(indexGroup);

  const h = blake2b(HASH_OUTPUT_LEN, null, null, personal);
  h.update(input);
  h.update(nonce);
  h.update(indexBuf);
  const full: Buffer = h.digest(Buffer.alloc(HASH_OUTPUT_LEN));

  const offset = (index % INDICES_PER_HASH) * N_BYTES;
  return full.subarray(offset, offset + N_BYTES);
}

function compressIndicesJS(indices: number[]): Buffer {
  const totalBits = BITS_PER_INDEX * indices.length;
  const out = Buffer.alloc(Math.ceil(totalBits / 8));
  let pos = 0;
  for (const val of indices) {
    for (let b = BITS_PER_INDEX - 1; b >= 0; b--) {
      const bit = (val >> b) & 1;
      out[pos >> 3] |= bit << (7 - (pos & 7));
      pos++;
    }
  }
  return out;
}

interface JSRow { hash: Buffer; indices: number[]; }

function solveJS(input: Buffer, nonce: Buffer, target: Buffer): Buffer | null {
  const rows: JSRow[] = new Array(NUM_INITIAL);
  for (let i = 0; i < NUM_INITIAL; i++) {
    rows[i] = { hash: generateLeafJS(input, nonce, i), indices: [i] };
  }

  let current = rows;
  for (let round = 0; round < K; round++) {
    current.sort((a, b) => a.hash.compare(b.hash));
    const next: JSRow[] = [];
    let i = 0;
    while (i < current.length) {
      let j = i + 1;
      while (j < current.length &&
             current[i].hash[0] === current[j].hash[0] &&
             current[i].hash[1] === current[j].hash[1]) j++;
      const bSize = Math.min(j - i, MAX_BUCKET);
      for (let ia = i; ia < i + bSize; ia++) {
        for (let ib = ia + 1; ib < i + bSize; ib++) {
          let distinct = true;
          for (const x of current[ia].indices) {
            if (current[ib].indices.includes(x)) { distinct = false; break; }
          }
          if (!distinct) continue;
          const hashLen = current[ia].hash.length;
          const newLen = hashLen - COLLISION_BYTES;
          const newHash = Buffer.alloc(Math.max(0, newLen));
          for (let k = COLLISION_BYTES; k < hashLen; k++) {
            newHash[k - COLLISION_BYTES] = current[ia].hash[k] ^ current[ib].hash[k];
          }
          const merged = current[ia].indices[0] < current[ib].indices[0]
            ? [...current[ia].indices, ...current[ib].indices]
            : [...current[ib].indices, ...current[ia].indices];
          next.push({ hash: newHash, indices: merged });
        }
      }
      i = j;
    }
    current = next;
    if (current.length === 0) return null;
  }

  for (const row of current) {
    if (row.indices.length !== SOLUTION_COUNT) continue;
    if (!row.hash.every((b: number) => b === 0)) continue;
    const compressed = compressIndicesJS(row.indices);
    const solHash = solutionHash(compressed, input);
    if (hashUnderTarget(solHash, target)) return compressed;
  }
  return null;
}

/**
 * Public: Single-nonce solve (JS fallback, used by benchmarks)
 */
export async function solveEquihashNative(
  n: number, k: number,
  input: Buffer, nonce: Buffer, target: Buffer
): Promise<Buffer | null> {
  if (!initialized) initSolver();
  return solveJS(input, nonce, target);
}

/**
 * Main entry point: multi-threaded solve via persistent native worker.
 */
export async function solveEquihashMulti(
  input: Buffer,
  target: Buffer,
  numThreads: number = 4,
  maxNonces: number = 5000
): Promise<{ nonce: Buffer; solution: Buffer; attempts: number; hps?: number } | null> {
  if (!initialized) initSolver();

  if (useNative && workerProc && !workerProc.killed) {
    try {
      const result = await solveViaWorker(input, target, numThreads, maxNonces);
      if (result) {
        return {
          nonce: result.nonce,
          solution: result.solution,
          attempts: result.attempts,
          hps: result.hps,
        };
      }
      return null;
    } catch (err: any) {
      console.log(`  ⚠ Worker error: ${err.message}, restarting...`);
      // Try to restart worker
      if (workerProc) {
        try { workerProc.kill("SIGTERM"); } catch {}
        workerProc = null;
      }
      startWorker();
      return null;
    }
  }

  // JS fallback
  for (let i = 0; i < maxNonces; i++) {
    const nonce = randomBytes(32);
    const result = solveJS(input, nonce, target);
    if (result) return { nonce, solution: result, attempts: i + 1 };
  }
  return null;
}
